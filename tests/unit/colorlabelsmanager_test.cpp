#include <catch2/catch.hpp>

#include <QSettings>
#include <QTemporaryDir>

#include "colorlabelsmanager.h"
#include "highlighterset.h"

SCENARIO( "color labels keep their stored match options", "[colorlabels]" )
{
    ColorLabelsManager colorLabelsManager;

    WHEN( "new labels are added with different defaults" )
    {
        auto labels
            = colorLabelsManager.setColorLabel( 0, QStringLiteral( "Error" ), { false, true } );
        labels
            = colorLabelsManager.setColorLabel( 1, QStringLiteral( "Warning" ), { true, false } );

        THEN( "each label stores the defaults used at creation time" )
        {
            REQUIRE( labels[ 0 ].size() == 1 );
            CHECK( labels[ 0 ].front().text == QStringLiteral( "Error" ) );
            CHECK_FALSE( labels[ 0 ].front().ignoreCase );
            CHECK( labels[ 0 ].front().wholeWord );

            REQUIRE( labels[ 1 ].size() == 1 );
            CHECK( labels[ 1 ].front().text == QStringLiteral( "Warning" ) );
            CHECK( labels[ 1 ].front().ignoreCase );
            CHECK_FALSE( labels[ 1 ].front().wholeWord );
        }
    }

    WHEN( "an existing label is moved to another color" )
    {
        auto labels
            = colorLabelsManager.setColorLabel( 0, QStringLiteral( "Error" ), { false, true } );
        labels = colorLabelsManager.setColorLabel( 1, QStringLiteral( "Error" ), { true, false } );

        THEN( "the original match options are preserved" )
        {
            CHECK( labels[ 0 ].isEmpty() );
            REQUIRE( labels[ 1 ].size() == 1 );
            CHECK( labels[ 1 ].front().text == QStringLiteral( "Error" ) );
            CHECK_FALSE( labels[ 1 ].front().ignoreCase );
            CHECK( labels[ 1 ].front().wholeWord );
        }
    }

    WHEN( "removing a color label by selected text" )
    {
        auto labels
            = colorLabelsManager.setColorLabel( 0, QStringLiteral( "Error" ), { false, true } );
        labels
            = colorLabelsManager.setColorLabel( 1, QStringLiteral( "Warning" ), { true, true } );

        labels = colorLabelsManager.removeColorLabel( QStringLiteral( "warning" ) );

        THEN( "only entries matching under their stored options are removed" )
        {
            REQUIRE( labels[ 0 ].size() == 1 );
            CHECK( labels[ 0 ].front().text == QStringLiteral( "Error" ) );
            CHECK( labels[ 1 ].isEmpty() );
        }
    }

    WHEN( "checking the current color for text" )
    {
        colorLabelsManager.setColorLabel( 0, QStringLiteral( "Error" ), { true, true } );
        colorLabelsManager.setColorLabel( 1, QStringLiteral( "Warning" ), { false, true } );

        THEN( "stored match options drive the lookup" )
        {
            CHECK( colorLabelsManager.currentColorLabelForText( QStringLiteral( "error" ) )
                   == std::optional<size_t>{ 0 } );
            CHECK( colorLabelsManager.currentColorLabelForText( QStringLiteral( "warning" ) )
                   == std::nullopt );
            CHECK( colorLabelsManager.currentColorLabelForText( QStringLiteral( "Warning" ) )
                   == std::optional<size_t>{ 1 } );
        }
    }
}

SCENARIO( "color labels apply to several texts at once", "[colorlabels]" )
{
    ColorLabelsManager colorLabelsManager;

    WHEN( "several texts are labelled in one action" )
    {
        auto labels = colorLabelsManager.setColorLabel(
            0, QStringList{ QStringLiteral( "alpha" ), QStringLiteral( "beta" ) },
            { false, true } );

        THEN( "each text gets its own entry with the given options" )
        {
            REQUIRE( labels[ 0 ].size() == 2 );
            CHECK( labels[ 0 ].at( 0 ).text == QStringLiteral( "alpha" ) );
            CHECK( labels[ 0 ].at( 1 ).text == QStringLiteral( "beta" ) );
            CHECK( labels[ 0 ].at( 0 ).wholeWord );
            CHECK( labels[ 0 ].at( 1 ).wholeWord );
        }
    }

    WHEN( "the bulk input contains duplicates and empty texts" )
    {
        auto labels = colorLabelsManager.setColorLabel(
            0,
            QStringList{ QStringLiteral( "alpha" ), QStringLiteral( "alpha" ), QString(),
                         QStringLiteral( "beta" ), QStringLiteral( "alpha" ) },
            { false, false } );

        THEN( "duplicates collapse and empty texts are skipped" )
        {
            REQUIRE( labels[ 0 ].size() == 2 );
            CHECK( labels[ 0 ].at( 0 ).text == QStringLiteral( "alpha" ) );
            CHECK( labels[ 0 ].at( 1 ).text == QStringLiteral( "beta" ) );
        }
    }

    WHEN( "one of several labelled texts is moved to another label" )
    {
        colorLabelsManager.setColorLabel(
            0, QStringList{ QStringLiteral( "alpha" ), QStringLiteral( "beta" ) },
            { false, true } );
        auto labels
            = colorLabelsManager.setColorLabel( 1, QStringList{ QStringLiteral( "alpha" ) },
                                                { true, false } );

        THEN( "only that text moves, keeping its stored options" )
        {
            REQUIRE( labels[ 0 ].size() == 1 );
            CHECK( labels[ 0 ].front().text == QStringLiteral( "beta" ) );
            REQUIRE( labels[ 1 ].size() == 1 );
            CHECK( labels[ 1 ].front().text == QStringLiteral( "alpha" ) );
            CHECK_FALSE( labels[ 1 ].front().ignoreCase );
            CHECK( labels[ 1 ].front().wholeWord );
        }
    }

    WHEN( "removing by several texts at once" )
    {
        colorLabelsManager.setColorLabel(
            0, QStringList{ QStringLiteral( "alpha" ), QStringLiteral( "beta" ) },
            { false, false } );
        colorLabelsManager.setColorLabel( 1, QStringList{ QStringLiteral( "gamma" ) },
                                          { false, false } );

        auto labels = colorLabelsManager.removeColorLabel(
            QStringList{ QStringLiteral( "alpha" ), QStringLiteral( "gamma" ) } );

        THEN( "every matching entry is removed from all labels" )
        {
            REQUIRE( labels[ 0 ].size() == 1 );
            CHECK( labels[ 0 ].front().text == QStringLiteral( "beta" ) );
            CHECK( labels[ 1 ].isEmpty() );
        }
    }

    WHEN( "a bulk selection exceeds the entry cap" )
    {
        QStringList many;
        for ( int i = 0; i < static_cast<int>( ColorLabelsManager::MaxBulkLabelTexts ) + 10;
              ++i ) {
            many.append( QStringLiteral( "line %1" ).arg( i ) );
        }

        auto labels = colorLabelsManager.setColorLabel( 0, many, { false, false } );

        THEN( "entries are truncated to the cap" )
        {
            CHECK( labels[ 0 ].size()
                   == static_cast<int>( ColorLabelsManager::MaxBulkLabelTexts ) );
        }
    }
}

SCENARIO( "next color label applies one label to the whole selection", "[colorlabels]" )
{
    // The cycle is read from HighlighterSetCollection: initialize the
    // persistable and seed a deterministic 3-label cycle IN MEMORY ONLY (no
    // save), restoring the previous configuration on exit.
    auto& highlighterCollection = HighlighterSetCollection::getSynced();
    struct QuickHighlightersGuard {
        ~QuickHighlightersGuard()
        {
            HighlighterSetCollection::get().setQuickHighlighters( saved );
        }
        QList<QuickHighlighter> saved;
    } guard{ highlighterCollection.quickHighlighters() };

    QList<QuickHighlighter> cycleConfig;
    for ( int i = 0; i < 3; ++i ) {
        cycleConfig.append(
            QuickHighlighter{ QStringLiteral( "ql%1" ).arg( i ), HighlightColor{}, true } );
    }
    highlighterCollection.setQuickHighlighters( cycleConfig );

    ColorLabelsManager colorLabelsManager;

    WHEN( "cycling with several texts" )
    {
        const QStringList texts{ QStringLiteral( "alpha" ), QStringLiteral( "beta" ) };
        auto labels = colorLabelsManager.setNextColorLabel( texts, { false, false } );

        THEN( "all texts land under the first cycle label" )
        {
            REQUIRE( labels[ 0 ].size() == 2 );
            CHECK( labels[ 1 ].isEmpty() );
            CHECK( labels[ 2 ].isEmpty() );
        }

        AND_THEN( "cycling again moves the whole selection together by one step" )
        {
            labels = colorLabelsManager.setNextColorLabel( texts, { false, false } );

            CHECK( labels[ 0 ].isEmpty() );
            REQUIRE( labels[ 1 ].size() == 2 );
            CHECK( labels[ 2 ].isEmpty() );

            AND_THEN( "the cycle wraps around for the whole selection" )
            {
                labels = colorLabelsManager.setNextColorLabel( texts, { false, false } );
                labels = colorLabelsManager.setNextColorLabel( texts, { false, false } );

                REQUIRE( labels[ 0 ].size() == 2 );
                CHECK( labels[ 1 ].isEmpty() );
                CHECK( labels[ 2 ].isEmpty() );
            }
        }
    }
}

SCENARIO( "quick color label defaults persist in highlighter settings", "[colorlabels]" )
{
    QTemporaryDir temporaryDir;
    REQUIRE( temporaryDir.isValid() );

    const auto settingsPath = temporaryDir.filePath( QStringLiteral( "highlighters.ini" ) );
    QSettings settings{ settingsPath, QSettings::IniFormat };

    WHEN( "defaults are saved and reloaded" )
    {
        HighlighterSetCollection savedCollection;
        savedCollection.setQuickHighlighterDefaults( { true, false } );
        savedCollection.saveToStorage( settings );
        settings.sync();

        HighlighterSetCollection loadedCollection;
        loadedCollection.retrieveFromStorage( settings );

        THEN( "the saved defaults are restored" )
        {
            const auto defaults = loadedCollection.quickHighlighterDefaults();
            CHECK( defaults.ignoreCase );
            CHECK_FALSE( defaults.wholeWord );
        }
    }

    WHEN( "settings have no stored quick defaults" )
    {
        HighlighterSetCollection loadedCollection;
        loadedCollection.retrieveFromStorage( settings );

        THEN( "the product defaults are used" )
        {
            const auto defaults = loadedCollection.quickHighlighterDefaults();
            CHECK_FALSE( defaults.ignoreCase );
            CHECK_FALSE( defaults.wholeWord );
        }
    }
}
