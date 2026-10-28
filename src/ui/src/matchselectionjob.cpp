#include "matchselectionjob.h"

#include <exception>
#include <utility>

namespace klogg {

MatchSelectionJob::MatchSelectionJob( LogSnapshot::Ptr source, LogSnapshot::Ptr presentation,
                                      QRegularExpression expression,
                                      std::shared_ptr<std::atomic<bool>> cancelled )
    : source_( std::move( source ) ), presentation_( std::move( presentation ) ),
      expression_( std::move( expression ) ), cancelled_( std::move( cancelled ) )
{
}

MatchSelectionStep MatchSelectionJob::step()
{
    MatchSelectionStep result;
    try {
        unsigned rows = 0;
        unsigned occurrences = 0;
        while ( nextRow_ < source_->lineCount() && rows < 128 && occurrences < 1024
                && !cancelled_->load( std::memory_order_relaxed ) ) {
            if ( !readingLine_ ) {
                if ( !source_->row( nextRow_ ).copyable ) {
                    ++nextRow_;
                    ++rows;
                    continue;
                }
                text_ = source_->lineString( nextRow_ );
                columns_ = MatchTextColumns{ text_ };
                iterator_ = expression_.globalMatch( text_ );
                readingLine_ = true;
            }
            if ( !iterator_.hasNext() ) {
                readingLine_ = false;
                ++nextRow_;
                ++rows;
                continue;
            }
            const auto occurrence = projectMatchOccurrence( iterator_.next() );
            const auto row = source_->row( nextRow_ );
            const auto presented = presentation_->presentedRowForIdentity( row.identity );
            for ( const auto& fragment : occurrence.fragments ) {
                const auto end = fragment.start + fragment.length;
                portions_.push_back( {
                    Portion{ nextRow_, columns_.displayColumn( fragment.start ),
                             columns_.displayColumn( end ) - 1_length,
                             fragment.start, end - 1_length },
                    row.sourceLineNumber,
                    text_.mid( fragment.start.get(), fragment.length.get() ),
                    row.identity, presented } );
            }
            ++occurrences;
        }
        result.processed = nextRow_.get();
        if ( nextRow_ >= source_->lineCount()
             && !cancelled_->load( std::memory_order_relaxed ) ) {
            result.selection = std::make_shared<Selection>();
            result.selection->selectCapturedPortions( std::move( portions_ ) );
            result.outcome = MatchSelectionStep::Outcome::Complete;
        }
    }
    catch ( const std::exception& error ) {
        result.outcome = MatchSelectionStep::Outcome::Failed;
        result.error = QString::fromUtf8( error.what() );
    }
    catch ( ... ) {
        result.outcome = MatchSelectionStep::Outcome::Failed;
        result.error = QStringLiteral( "Unknown match extraction failure" );
    }
    return result;
}

} // namespace klogg
