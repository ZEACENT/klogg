#ifndef TEST_UTILS_H
#define TEST_UTILS_H

#include <chrono>
#include <memory>
#include <string>

#include <QSignalSpy>
#include <QTest>

#include <catch2/catch.hpp>

#include <configuration.h>

// Soft precondition for environment-dependent tests.  Use in place of REQUIRE
// when the predicate is checking something the test environment is supposed
// to provide (an installed external tool, a runner-specific assumption, etc.)
// rather than the production code under test.  When the predicate is false,
// emits a Catch2 WARN with the supplied message and returns from the
// enclosing function -- the SCENARIO is silently skipped instead of failing
// CI.  Mechanises the inline `WARN(...); return;` pattern already used at
// several existing skip-points in tests/unit/adb_ui_transport_test.cpp.
#define KLOGG_REQUIRE_OR_WARN_SKIP( cond, msg )                                                    \
    do {                                                                                            \
        if ( !( cond ) ) {                                                                          \
            WARN( msg );                                                                            \
            return;                                                                                 \
        }                                                                                           \
    } while ( 0 )
/*
struct TestTimer {
    TestTimer()
        : TestTimer(
                ::testing::UnitTest::GetInstance()->current_test_info()->test_case_name() ) {
    text_ += std::string {"."} + std::string {::testing::UnitTest::GetInstance()->current_test_info()->name() };
    }

    TestTimer(const std::string& text)
        : Start { std::chrono::system_clock::now() }
        , text_ {text} {}

    virtual ~TestTimer() {
        using namespace std;
        Stop = chrono::system_clock::now();
        Elapsed = chrono::duration_cast<chrono::microseconds>(Stop - Start);
        cout << endl << text_ << " elapsed time = "
            << Elapsed.count() * 0.001 << "ms" << endl;
    }

    std::chrono::time_point<std::chrono::system_clock> Start;
    std::chrono::time_point<std::chrono::system_clock> Stop;
    std::chrono::microseconds Elapsed;
    std::string text_;
};
*/
class SafeQSignalSpy {
  public:
    template <typename... Args>
    SafeQSignalSpy( Args&&... agruments )
        : spy_( std::make_unique<QSignalSpy>( std::forward<Args>( agruments )... ) )
    {
    }

    ~SafeQSignalSpy()
    {
        if ( !spy_ ) {
            return;
        }
#ifdef Q_OS_WIN
        // QSignalSpy teardown can crash in Windows CI/local runs when the sender is
        // being destroyed concurrently during test unwinding. The processes are
        // short-lived; leaking the spy object avoids the flaky destructor path.
        (void)spy_.release();
#endif
    }

    SafeQSignalSpy( const SafeQSignalSpy& ) = delete;
    SafeQSignalSpy& operator=( const SafeQSignalSpy& ) = delete;

    SafeQSignalSpy( SafeQSignalSpy&& ) = delete;
    SafeQSignalSpy& operator=( SafeQSignalSpy&& ) = delete;

    int count() const
    {
        return spy_ ? spy_->count() : 0;
    }

    bool wait( int timeout = 5000 )
    {
        return spy_ && spy_->wait( timeout );
    }

    QList<QVariant> at( int i ) const
    {
        return spy_ ? spy_->at( i ) : QList<QVariant>{};
    }

    QList<QVariant> takeFirst()
    {
        return spy_ ? spy_->takeFirst() : QList<QVariant>{};
    }

    void clear()
    {
        if ( spy_ ) {
            spy_->clear();
        }
    }

    bool isValid() const
    {
        return spy_ && spy_->isValid();
    }

    bool safeWait( int timeout = 10000 ) {
        // If it has already been received
        bool result = count() > 0;
        if ( ! result ) {
            result = wait( timeout );
        }
        return result;
    }

  private:
    std::unique_ptr<QSignalSpy> spy_;
};

inline void configureProductLikeRegexpEngine( Configuration& config )
{
#ifdef KLOGG_HAS_VECTORSCAN
    config.setRegexpEnging( RegexpEngine::Vectorscan );
#else
    config.setRegexpEnging( RegexpEngine::QRegularExpression );
#endif
}

class ScopedRegexpEngine {
  public:
    explicit ScopedRegexpEngine( RegexpEngine engine )
        : config_( Configuration::getSynced() )
        , previousEngine_( config_.regexpEngine() )
    {
        config_.setRegexpEnging( engine );
    }

    ~ScopedRegexpEngine()
    {
        config_.setRegexpEnging( previousEngine_ );
    }

    ScopedRegexpEngine( const ScopedRegexpEngine& ) = delete;
    ScopedRegexpEngine& operator=( const ScopedRegexpEngine& ) = delete;

    ScopedRegexpEngine( ScopedRegexpEngine&& ) = delete;
    ScopedRegexpEngine& operator=( ScopedRegexpEngine&& ) = delete;

  private:
    Configuration& config_;
    RegexpEngine previousEngine_;
};

template<typename F>
bool waitUiState(F&& checkFunc ) {
    for ( auto time = 0; time < 10000; time += 100 ) {
        if ( checkFunc() ) {
            return true;
        }
        QTest::qWait( 100 );
    }
    return false;
};

#endif
