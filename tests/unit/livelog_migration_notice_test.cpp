/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Task 6 cycle 2 RED contract: migration notices.
 *
 * The one-time pre-discriminator migration diagnostic must reach the user as
 * a NON-error notice, exactly once per capture id — never through the
 * rejection/warning channel that un-restorable sessions use.
 *
 * This file intentionally does not compile yet: Session/WindowSession expose
 * lastRestoreRejections() but no notice channel, so the assertions below
 * reference the missing lastRestoreNotices() surface. The build break IS the
 * cycle 2 RED state for this clause; the GREEN step adds the accessor (and
 * the once-per-capture-id dedup behind it) without touching these tests.
 */

#include <catch2/catch.hpp>

#include <memory>
#include <utility>
#include <vector>

#include <QString>

#include "livelogsession.h"
#include "livesourcetransport.h"
#include "session.h"
#include "sessioninfo.h"
#include "viewinterface.h"

namespace {

constexpr auto FirstCaptureId = "8b0d2f4a-6c1e-4a7b-9d3f-5c7e9a1b3d4f";
constexpr auto SecondCaptureId = "a2c4e6b0-8d1f-4a3c-be5e-7f9a2c4e6b81";

// Minimal view stand-in (no widget needed to observe notice surfacing).
class NullView final : public ViewInterface {
protected:
    void doSetData( std::shared_ptr<SearchableLogData>, std::shared_ptr<LogFilteredData> ) override
    {
    }
    void doSetQuickFindPattern( std::shared_ptr<QuickFindPattern> ) override {}
    void doSetSavedSearches( SavedSearches* ) override {}
    void doSetViewContext( const QString& ) override {}
    std::shared_ptr<const ViewContextInterface> doGetViewContext() const override
    {
        return nullptr;
    }
};

// Removes every persisted window for the duration of one test case.
class ScopedSessionWindows {
public:
    ScopedSessionWindows()
    {
        clearAll();
    }

    ~ScopedSessionWindows()
    {
        clearAll();
    }

    ScopedSessionWindows( const ScopedSessionWindows& ) = delete;
    ScopedSessionWindows& operator=( const ScopedSessionWindows& ) = delete;

private:
    static void clearAll()
    {
        auto& sessionInfo = SessionInfo::getSynced();
        for ( const auto& windowId : sessionInfo.windows() ) {
            sessionInfo.remove( windowId );
        }
        sessionInfo.save();
    }
};

QString flatAdbPayload( const char* captureId, const QString& description )
{
    return QStringLiteral(
               R"json({"sourceType":"adb_logcat","deviceSerial":"R58NC123ABC","deviceDescription":"%1","captureId":"%2"})json" )
        .arg( description, QLatin1String{ captureId } );
}

QString flatIosPayload( const char* captureId, const QString& description )
{
    return QStringLiteral(
               R"json({"sourceType":"ios_log_stream","deviceSerial":"00008101-001A2B3C4D5E","deviceDescription":"%1","captureId":"%2"})json" )
        .arg( description, QLatin1String{ captureId } );
}

void seedSingleFile( const QString& windowId, const QString& payload )
{
    auto& sessionInfo = SessionInfo::getSynced();
    sessionInfo.add( windowId );
    sessionInfo.setOpenFiles(
        windowId, { SessionInfo::OpenFile{ QStringLiteral( "Pixel 8 Pro" ), 0, QString{},
                                           QStringLiteral( "adb_logcat" ),
                                           QStringLiteral( "Pixel 8 Pro" ), payload } } );
    sessionInfo.setCurrentFileIndex( windowId, 0 );
    sessionInfo.save();
}

OpenedDocumentsList restoreWindow( WindowSession& windowSession )
{
    int currentIndex = -1;
    return windowSession.restore(
        []() -> ViewInterface* {
            // Ownership transfers to the session.
            // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
            return new NullView();
        },
        &currentIndex );
}

void closeAndDeleteViews( Session& appSession, OpenedDocumentsList& opened )
{
    for ( auto& entry : opened ) {
        appSession.close( entry.second );
        delete entry.second;
    }
    opened.clear();
}

} // namespace

SCENARIO( "Migration notices surface once per capture id as non-errors",
          "[livelog-migration-notice][session]" )
{
    ScopedSessionWindows windowsGuard;

    auto appSession = std::make_shared<Session>();

    const auto windowId = QStringLiteral( "livelog-migration-window" );

    GIVEN( "A pre-discriminator session payload" )
    {
        // Pure anchor: parsing reports the one-time migration as INFO and
        // migrates forward to the built-in backend (cycle 1 semantics).
        const auto payload = flatAdbPayload( FirstCaptureId, QStringLiteral( "Pixel 8 Pro" ) );
        const auto parsed = klogg::livelog::parsePersistedSpec( payload );
        REQUIRE( parsed.ok() );
        REQUIRE( parsed.spec->androidBackend == klogg::livelog::AndroidBackend::SmartSocket );
        REQUIRE( parsed.spec->migratedFromLegacySession );

        WHEN( "The session is restored" )
        {
            seedSingleFile( windowId, payload );
            WindowSession firstPass{ appSession, windowId, 0 };
            auto opened = restoreWindow( firstPass );

            THEN( "Exactly one non-error migration notice is surfaced" )
            {
                const auto notices = appSession->lastRestoreNotices();
                REQUIRE( notices.size() == 1 );
                REQUIRE(
                    notices.front().contains( QStringLiteral( "migrat" ), Qt::CaseInsensitive ) );
                REQUIRE( appSession->lastRestoreRejections().isEmpty() );
            }

            closeAndDeleteViews( *appSession, opened );

            AND_WHEN( "The same capture id is restored again" )
            {
                seedSingleFile( windowId, payload );
                WindowSession secondPass{ appSession, windowId, 0 };
                auto reopened = restoreWindow( secondPass );

                THEN( "No second notice is produced for that capture id" )
                {
                    REQUIRE( appSession->lastRestoreNotices().isEmpty() );
                    REQUIRE( appSession->lastRestoreRejections().isEmpty() );
                }

                closeAndDeleteViews( *appSession, reopened );

                AND_WHEN( "A different pre-discriminator capture id is restored" )
                {
                    seedSingleFile( windowId, flatAdbPayload( SecondCaptureId,
                                                              QStringLiteral( "Pixel 8 Pro" ) ) );
                    WindowSession thirdPass{ appSession, windowId, 0 };
                    auto thirdOpened = restoreWindow( thirdPass );

                    THEN( "It gets its own single notice" )
                    {
                        const auto notices = appSession->lastRestoreNotices();
                        REQUIRE( notices.size() == 1 );
                        REQUIRE( notices.front().contains( QStringLiteral( "migrat" ),
                                                           Qt::CaseInsensitive ) );
                        REQUIRE( appSession->lastRestoreRejections().isEmpty() );
                    }

                    closeAndDeleteViews( *appSession, thirdOpened );
                }
            }
        }
    }
}

TEST_CASE( "Migration notice identity includes the source document, not capture id alone",
           "[livelog-migration-notice][session]" )
{
    ScopedSessionWindows windowsGuard;
    auto appSession = std::make_shared<Session>();
    const auto windowId = QStringLiteral( "livelog-migration-collision-window" );

    auto& sessionInfo = SessionInfo::getSynced();
    sessionInfo.add( windowId );
    sessionInfo.setOpenFiles(
        windowId,
        { SessionInfo::OpenFile{
              QStringLiteral( "Pixel collision" ), 0, QString{}, QStringLiteral( "adb_logcat" ),
              QStringLiteral( "Pixel collision" ),
              flatAdbPayload( FirstCaptureId, QStringLiteral( "Pixel collision" ) ) },
          SessionInfo::OpenFile{
              QStringLiteral( "iPhone collision" ), 0, QString{},
              QStringLiteral( "ios_log_stream" ), QStringLiteral( "iPhone collision" ),
              flatIosPayload( FirstCaptureId, QStringLiteral( "iPhone collision" ) ) } } );
    sessionInfo.setCurrentFileIndex( windowId, 0 );
    sessionInfo.save();

    WindowSession windowSession{ appSession, windowId, 0 };
    auto opened = restoreWindow( windowSession );

    // adb://id and ios-log://id are distinct notice identities even when an
    // imported session reuses the same storage id. The second tab is rejected
    // to protect capture storage, but neither migration notice may hide the other.
    REQUIRE( opened.size() == 1 );
    REQUIRE( appSession->lastRestoreNotices().size() == 2 );
    REQUIRE( appSession->lastRestoreRejections().size() == 1 );

    closeAndDeleteViews( *appSession, opened );
}
