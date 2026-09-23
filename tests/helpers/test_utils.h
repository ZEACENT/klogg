#ifndef TEST_UTILS_H
#define TEST_UTILS_H

#include <chrono>
#include <memory>
#include <string>

#include <QByteArray>
#include <QSignalSpy>
#include <QTest>

#include <catch2/catch.hpp>

#include <configuration.h>
#include <platform/platform_input.h>
#include <shortcuts.h>

// Simulate pressing the key CURRENTLY configured for `action` (defaults merged
// with the machine's saved overrides). Hardcoding the default key is brittle:
// a developer machine that rebound the action would make the keyClick miss the
// registered shortcut even though the wiring is correct.
inline void pressConfiguredShortcut( QWidget* target, const std::string& action )
{
    const auto& configured = Configuration::get().shortcuts();
    const auto keys = ShortcutAction::shortcutKeys( action, configured );
    REQUIRE( !keys.isEmpty() );
    const auto sequence = QKeySequence( keys.front() );
    REQUIRE( !sequence.isEmpty() );
    // A QKeySequence can hold multiple chords (e.g. "Ctrl+K, Ctrl+X"); send
    // every chord so multi-chord bindings actually fire. Single-chord
    // sequences (the common case) loop exactly once.
    for ( int chord = 0; chord < sequence.count(); ++chord ) {
        const auto combined = klogg::platform::keyChordToCombined( sequence, chord );
        const auto key = static_cast<Qt::Key>(
            combined & ~static_cast<int>( Qt::KeyboardModifierMask ) );
        const auto modifiers = static_cast<Qt::KeyboardModifiers>(
            combined & static_cast<int>( Qt::KeyboardModifierMask ) );
        QTest::keyClick( target, key, modifiers );
        QTest::qWait( 20 );
    }
}

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
// RAII env var guard for tests: restores the previous value (or unsets) on
// scope exit. Tests that mutate process-global environment must never leak
// the mutation into later cases -- under ctest --parallel the per-process
// isolation variables (KLOGG_CAPTURE_COORDINATION_ROOT,
// KLOGG_PORTABLE_CONFIG_DIR) are set by ctest, and clobbering them silently
// re-enables cross-process state sharing for the rest of the binary.
class ScopedEnvironmentVariable final {
  public:
    ScopedEnvironmentVariable( QByteArray name, const QByteArray& value )
        : name_( std::move( name ) )
        , wasSet_( qEnvironmentVariableIsSet( name_.constData() ) )
        , previous_( qgetenv( name_.constData() ) )
    {
        qputenv( name_.constData(), value );
    }

    ~ScopedEnvironmentVariable()
    {
        if ( wasSet_ ) {
            qputenv( name_.constData(), previous_ );
        }
        else {
            qunsetenv( name_.constData() );
        }
    }

    ScopedEnvironmentVariable( const ScopedEnvironmentVariable& ) = delete;
    ScopedEnvironmentVariable& operator=( const ScopedEnvironmentVariable& ) = delete;

  private:
    QByteArray name_;
    bool wasSet_{ false };
    QByteArray previous_;
};

// Performance budgets must never gate CI on runner speed. Budget assertions
// go through KLOGG_CHECK_PERF_BUDGET; report measurements separately with
// CAPTURE or INFO when needed. The expression is only evaluated and the
// assertion only fires when a developer opts in locally with
// KLOGG_PERF_GATES=1 -- scripts/run_perf_gates.py sets it. CI never sets the
// variable, so a slow or loaded hosted runner cannot flake the merge gate on
// a wall-clock threshold.
//
// The flip side: an expression behind this macro is NOT checked in CI. Mark
// every call site with '// lint-allow: perf-budget -- <nonempty reason>' in
// a comment within the assertion's line span. The determinism lint requires
// the reason on the marker's own line; explain why skipping the budget in CI
// is safe, and route only genuine speed budgets through this macro. A
// correctness or liveness property -- "this call only dispatches and never
// runs the work on the caller's thread", "the contended lock waited for the
// configured timeout" -- must be asserted deterministically (observe the
// mechanism: the executing thread, the effective timeout) so every CI leg
// checks it; see docs/BUILD.md.
inline bool perfGatesEnabled()
{
    return qgetenv( "KLOGG_PERF_GATES" ) == "1";
}

#define KLOGG_CHECK_PERF_BUDGET( expr )                                                            \
    do {                                                                                            \
        if ( perfGatesEnabled() ) {                                                                 \
            CHECK( expr );                                                                          \
        }                                                                                           \
    } while ( 0 )

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

inline constexpr int kAsyncCompletionTimeoutMs = 30000;

template <typename Sender, typename Signal, typename Trigger, typename Ready>
inline void triggerAndWaitForCompletion( Sender* sender, Signal signal, Trigger&& trigger,
                                         Ready&& ready,
                                         int timeoutMs = kAsyncCompletionTimeoutMs )
{
    SafeQSignalSpy completion( sender, signal );
    trigger();
    if ( !ready() ) {
        REQUIRE( completion.safeWait( timeoutMs ) );
    }
    REQUIRE( ready() );
}

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
