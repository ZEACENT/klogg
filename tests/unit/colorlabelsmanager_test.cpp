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
