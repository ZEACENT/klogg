#include <catch2/catch.hpp>

#include <atomic>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QObject>

class ConcurrentSignalEmitter final : public QObject {
    Q_OBJECT

  public:
    void emitProgress( quint64 requestId, int percent, quint64 matches )
    {
        Q_EMIT searchProgressed( requestId, percent, matches );
    }

  Q_SIGNALS:
    void searchProgressed( quint64 requestId, int percent, quint64 matches );
};

class ConcurrentSignalReceiver final : public QObject {
    Q_OBJECT

  public:
    int receivedSignals() const
    {
        return receivedSignals_;
    }

  public Q_SLOTS:
    void recordProgress( quint64, int, quint64 )
    {
        ++receivedSignals_;
    }

  private:
    int receivedSignals_ = 0;
};

namespace {
constexpr auto ConcurrentEventType = static_cast<QEvent::Type>( QEvent::User + 37 );

class CountingEventReceiver final : public QObject {
  public:
    int receivedEvents() const
    {
        return receivedEvents_;
    }

  protected:
    bool event( QEvent* event ) override
    {
        if ( event->type() == ConcurrentEventType ) {
            ++receivedEvents_;
            return true;
        }
        return QObject::event( event );
    }

  private:
    int receivedEvents_ = 0;
};
} // namespace

TEST_CASE( "Qt event queue serializes concurrent producers with main-thread delivery",
           "[qt][threading][tsan]" )
{
    CountingEventReceiver receiver;
    constexpr int producerCount = 4;
    constexpr int eventsPerProducer = 2000;
    constexpr int expectedEvents = producerCount * eventsPerProducer;

    std::atomic<int> producersDone{ 0 };
    std::vector<std::thread> producers;
    producers.reserve( producerCount );

    for ( int producer = 0; producer < producerCount; ++producer ) {
        producers.emplace_back( [ &receiver, &producersDone ] {
            for ( int event = 0; event < eventsPerProducer; ++event ) {
                // postEvent takes ownership of the heap event.
                // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
                QCoreApplication::postEvent( &receiver, new QEvent( ConcurrentEventType ) );
            }
            producersDone.fetch_add( 1 );
        } );
    }

    while ( producersDone.load() != producerCount ) {
        QCoreApplication::sendPostedEvents( &receiver, ConcurrentEventType );
    }

    for ( auto& producer : producers ) {
        producer.join();
    }
    QCoreApplication::sendPostedEvents( &receiver, ConcurrentEventType );

    REQUIRE( receiver.receivedEvents() == expectedEvents );
}

TEST_CASE( "Qt queued signal argument types publish across concurrent emitters",
           "[qt][threading][tsan]" )
{
    ConcurrentSignalEmitter emitter;
    ConcurrentSignalReceiver receiver;

    // Every connection has its own lazily initialized queued-argument metadata.
    // A wide first emission makes concurrent workers contend on many fresh
    // argumentTypes pointers instead of relying on one very short race window.
    constexpr int connectionCount = 128;
    for ( int connection = 0; connection < connectionCount; ++connection ) {
        const auto result
            = QObject::connect( &emitter, &ConcurrentSignalEmitter::searchProgressed,
                                &receiver, &ConcurrentSignalReceiver::recordProgress,
                                Qt::AutoConnection );
        REQUIRE( result );
    }

    constexpr int producerCount = 8;
    constexpr int signalsPerProducer = 4;
    constexpr int expectedSignals
        = producerCount * signalsPerProducer * connectionCount;

    std::atomic<bool> start{ false };
    std::atomic<int> producersDone{ 0 };
    std::vector<std::thread> producers;
    producers.reserve( producerCount );
    for ( int producer = 0; producer < producerCount; ++producer ) {
        producers.emplace_back( [ &emitter, &start, &producersDone, producer ] {
            while ( !start.load( std::memory_order_acquire ) ) {
                std::this_thread::yield();
            }
            for ( int signal = 0; signal < signalsPerProducer; ++signal ) {
                emitter.emitProgress( static_cast<quint64>( producer ), signal,
                                      static_cast<quint64>( signal ) );
            }
            producersDone.fetch_add( 1, std::memory_order_release );
        } );
    }

    start.store( true, std::memory_order_release );
    while ( producersDone.load( std::memory_order_acquire ) != producerCount ) {
        QCoreApplication::sendPostedEvents( &receiver );
    }
    for ( auto& producer : producers ) {
        producer.join();
    }

    QElapsedTimer deadline;
    deadline.start();
    while ( receiver.receivedSignals() != expectedSignals && deadline.elapsed() < 5000 ) {
        QCoreApplication::sendPostedEvents( &receiver );
    }

    REQUIRE( receiver.receivedSignals() == expectedSignals );
}

#include "qt_event_queue_test.moc"
