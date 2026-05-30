/*
 * Copyright (C) 2021 Anton Filimonov and other contributors
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

#include <QApplication>
#include <QEvent>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QTimer>
#include <QSettings>
#include <qcolor.h>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "configuration.h"
#include "log.h"
#include "styles.h"

static bool isSystemDarkTheme();

namespace {
enum class ThemeVariant {
    Light,
    Dark
};

struct ThemeTokens {
    QColor window;
    QColor windowText;
    QColor base;
    QColor alternateBase;
    QColor toolTipBase;
    QColor toolTipText;
    QColor text;
    QColor button;
    QColor buttonText;
    QColor link;
    QColor highlight;
    QColor highlightedText;
    QColor border;
    QColor mutedText;
    QColor inputBg;
    QColor inputBorder;
    QColor inputFocusBorder;
    QColor buttonHover;
    QColor buttonPressed;
    QColor accent;
    QColor accentHover;
    QColor accentPressed;
    QColor accentSoft;
    QColor menuBg;
    QColor menuHover;
    QColor toolbarGradientTop;
    QColor toolbarGradientBottom;
    QColor scrollbarTrack;
    QColor scrollbarThumb;
    QColor scrollbarThumbHover;
};

class ThemeWatcher : public QObject {
  public:
    explicit ThemeWatcher( QObject* parent = nullptr )
        : QObject( parent )
    {
#if QT_VERSION >= QT_VERSION_CHECK( 6, 5, 0 )
        if ( QGuiApplication::styleHints() ) {
            connect( QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
                     [ this ]( Qt::ColorScheme ) { handleThemeChange(); } );
        }
#endif
    }

  protected:
    bool eventFilter( QObject* obj, QEvent* event ) override
    {
        if ( event->type() == QEvent::ThemeChange ) {
            handleThemeChange();
        }
        return QObject::eventFilter( obj, event );
    }

  private:
    void handleThemeChange()
    {
        if ( Configuration::get().themeMode() != ThemeMode::Auto ) {
            return;
        }
        if ( pending_ ) {
            return;
        }
        pending_ = true;
        QTimer::singleShot( 0, qApp, [ this ]() {
            pending_ = false;
            if ( QCoreApplication::closingDown() ) {
                return;
            }
            if ( Configuration::get().themeMode() != ThemeMode::Auto ) {
                return;
            }
            StyleManager::applyStyle( Configuration::get().style() );
        } );
    }

    bool pending_ = false;
};

static void ensureThemeWatcher()
{
    static ThemeWatcher* watcher = nullptr;
    if ( watcher || !qApp ) {
        return;
    }
    watcher = new ThemeWatcher( qApp );
    qApp->installEventFilter( watcher );
}

static QString toCss( const QColor& color )
{
    return color.name( QColor::HexRgb );
}

static ThemeTokens modernLightTokens()
{
    return ThemeTokens{
        QColor( "#F5F6F8" ), // window
        QColor( "#1F2328" ), // windowText
        QColor( "#FFFFFF" ), // base
        QColor( "#EEF1F4" ), // alternateBase
        QColor( "#111827" ), // toolTipBase
        QColor( "#F9FAFB" ), // toolTipText
        QColor( "#1F2328" ), // text
        QColor( "#FFFFFF" ), // button
        QColor( "#1F2328" ), // buttonText
        QColor( "#2563EB" ), // link
        QColor( "#DCEBFF" ), // highlight
        QColor( "#0B1F44" ), // highlightedText
        QColor( "#D0D7DE" ), // border
        QColor( "#6B7280" ), // mutedText
        QColor( "#FFFFFF" ), // inputBg
        QColor( "#C9D1D9" ), // inputBorder
        QColor( "#2563EB" ), // inputFocusBorder
        QColor( "#F2F5F9" ), // buttonHover
        QColor( "#E8EDF3" ), // buttonPressed
        QColor( "#2563EB" ), // accent
        QColor( "#1D4ED8" ), // accentHover
        QColor( "#1E40AF" ), // accentPressed
        QColor( "#E6F0FF" ), // accentSoft
        QColor( "#FFFFFF" ), // menuBg
        QColor( "#EFF4FF" ), // menuHover
        QColor( "#FFFFFF" ), // toolbarGradientTop
        QColor( "#EEF2F6" ), // toolbarGradientBottom
        QColor( "#EEF1F5" ), // scrollbarTrack
        QColor( "#C6CDD6" ), // scrollbarThumb
        QColor( "#AEB7C3" ), // scrollbarThumbHover
    };
}

static ThemeTokens modernDarkTokens()
{
    return ThemeTokens{
        QColor( "#0F1115" ), // window
        QColor( "#E6E9EF" ), // windowText
        QColor( "#151821" ), // base
        QColor( "#1B1F2A" ), // alternateBase
        QColor( "#1A1F2B" ), // toolTipBase
        QColor( "#E6E9EF" ), // toolTipText
        QColor( "#E6E9EF" ), // text
        QColor( "#1A1F2B" ), // button
        QColor( "#E6E9EF" ), // buttonText
        QColor( "#5B8CFF" ), // link
        QColor( "#23314D" ), // highlight
        QColor( "#EAF2FF" ), // highlightedText
        QColor( "#2A2F3A" ), // border
        QColor( "#9AA3B2" ), // mutedText
        QColor( "#141722" ), // inputBg
        QColor( "#303646" ), // inputBorder
        QColor( "#5B8CFF" ), // inputFocusBorder
        QColor( "#222939" ), // buttonHover
        QColor( "#2A3346" ), // buttonPressed
        QColor( "#5B8CFF" ), // accent
        QColor( "#4C7DFF" ), // accentHover
        QColor( "#3B6EF0" ), // accentPressed
        QColor( "#1C2B47" ), // accentSoft
        QColor( "#151821" ), // menuBg
        QColor( "#1F2A3D" ), // menuHover
        QColor( "#1A1F2B" ), // toolbarGradientTop
        QColor( "#12151D" ), // toolbarGradientBottom
        QColor( "#0F1115" ), // scrollbarTrack
        QColor( "#2B3242" ), // scrollbarThumb
        QColor( "#3A4357" ), // scrollbarThumbHover
    };
}

static QPalette paletteFromTokens( const ThemeTokens& t )
{
    QPalette palette;
    palette.setColor( QPalette::Window, t.window );
    palette.setColor( QPalette::WindowText, t.windowText );
    palette.setColor( QPalette::Base, t.base );
    palette.setColor( QPalette::AlternateBase, t.alternateBase );
    palette.setColor( QPalette::ToolTipBase, t.toolTipBase );
    palette.setColor( QPalette::ToolTipText, t.toolTipText );
    palette.setColor( QPalette::Text, t.text );
    palette.setColor( QPalette::Button, t.button );
    palette.setColor( QPalette::ButtonText, t.buttonText );
    palette.setColor( QPalette::Link, t.link );
    palette.setColor( QPalette::Highlight, t.highlight );
    palette.setColor( QPalette::HighlightedText, t.highlightedText );
    palette.setColor( QPalette::Active, QPalette::Button, t.buttonHover );
    palette.setColor( QPalette::Disabled, QPalette::ButtonText, t.mutedText );
    palette.setColor( QPalette::Disabled, QPalette::WindowText, t.mutedText );
    palette.setColor( QPalette::Disabled, QPalette::Text, t.mutedText );
    return palette;
}

static QString buildModernStyleSheet( const ThemeTokens& t, bool dark )
{
    const QString spinArrowUp = dark ? QStringLiteral( ":/images/arrowup_inverse.svg" )
                                     : QStringLiteral( ":/images/arrowup.svg" );
    const QString spinArrowDown = dark ? QStringLiteral( ":/images/arrowdown_inverse.svg" )
                                       : QStringLiteral( ":/images/arrowdown.svg" );

    const QString closeButtonIcon
        = dark ? QStringLiteral( ":/images/icons8-close-window_inverse.svg" )
               : QStringLiteral( ":/images/icons8-close-window.svg" );
    const QString closeButtonHoverIcon
        = dark ? QStringLiteral( ":/images/icons8-close-window-hover_inverse.svg" )
               : QStringLiteral( ":/images/icons8-close-window-hover.svg" );

    QString style = QStringLiteral( R"(
QMainWindow, QDialog {
    background-color: __WINDOW__;
    color: __TEXT__;
}

QToolTip {
    background-color: __TOOLTIP_BG__;
    color: __TOOLTIP_TEXT__;
    border: 1px solid __BORDER__;
    padding: 4px 6px;
    border-radius: 6px;
}

QToolBar {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 __TOOLBAR_TOP__, stop:1 __TOOLBAR_BOTTOM__);
    border-bottom: 1px solid __BORDER__;
    spacing: 4px;
    padding: 2px;
}
QToolBar::separator {
    background: __BORDER__;
    width: 1px;
    margin: 2px 6px;
}

QMenuBar {
    background-color: __WINDOW__;
    padding: 2px 6px;
}
QMenuBar::item {
    padding: 4px 8px;
    border-radius: 4px;
}
QMenuBar::item:selected {
    background-color: __MENU_HOVER__;
}

QMenu {
    background-color: __MENU_BG__;
    border: 1px solid __BORDER__;
    border-radius: 6px;
    padding: 4px;
}
QMenu::item {
    padding: 4px 22px 4px 20px;
    border-radius: 4px;
}
QMenu::item:selected {
    background-color: __MENU_HOVER__;
}
QMenu::item:disabled {
    color: __MUTED__;
}
QMenu::separator {
    height: 1px;
    background: __BORDER__;
    margin: 6px 8px;
}

QStatusBar {
    background-color: __WINDOW__;
    border-top: 1px solid __BORDER__;
}

QTabWidget::pane {
    border: 0px;
    top: 0px;
    background-color: __BASE__;
}
QTabBar {
    background-color: __ALT_BASE__;
    border: 1px solid __BORDER__;
    border-radius: 14px;
    padding: 2px;
}
QTabBar::tab {
    background-color: transparent;
    padding: 3px 12px;
    border: 1px solid transparent;
    border-radius: 12px;
    margin: 2px 3px;
    min-height: 20px;
}
QTabBar::tab:selected {
    background-color: __BUTTON_PRESSED__;
    color: __TEXT__;
    border: 1px solid __BORDER__;
    font-weight: 600;
}
QTabBar::tab:!selected {
    color: __MUTED__;
}
QTabBar::tab:hover:!selected {
    background-color: __BUTTON_HOVER__;
    color: __TEXT__;
}
QTabBar::close-button {
    image: url(__TAB_CLOSE__);
    height: 12px;
    width: 12px;
    subcontrol-origin: padding;
    subcontrol-position: right;
}
QTabBar::close-button:hover {
    image: url(__TAB_CLOSE_HOVER__);
}

QGroupBox {
    margin-top: 10px;
    border: 1px solid __BORDER__;
    border-radius: 6px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 10px;
    padding: 0 6px;
    color: __MUTED__;
}

QLabel {
    color: __TEXT__;
}

QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QKeySequenceEdit,
QTextEdit, QPlainTextEdit, QAbstractSpinBox {
    background-color: __INPUT_BG__;
    border: 1px solid __INPUT_BORDER__;
    border-radius: 6px;
    padding: 3px 6px;
    min-height: 20px;
}
QSpinBox, QDoubleSpinBox {
    padding-right: 22px;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus,
QTextEdit:focus, QPlainTextEdit:focus, QAbstractSpinBox:focus {
    border: 1px solid __INPUT_FOCUS__;
    background-color: __BASE__;
}

QComboBox::drop-down {
    border: 0px;
    width: 18px;
}

QComboBox::down-arrow {
    image: url(__SPIN_DOWN__);
    width: 8px;
    height: 8px;
}

QSpinBox::up-button, QDoubleSpinBox::up-button {
    subcontrol-origin: border;
    subcontrol-position: top right;
    width: 20px;
    border-left: 1px solid __INPUT_BORDER__;
    border-top-right-radius: 6px;
    background-color: __BUTTON__;
}
QSpinBox::down-button, QDoubleSpinBox::down-button {
    subcontrol-origin: border;
    subcontrol-position: bottom right;
    width: 20px;
    border-left: 1px solid __INPUT_BORDER__;
    border-bottom-right-radius: 6px;
    background-color: __BUTTON__;
}
QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
    background-color: __BUTTON_HOVER__;
}
QSpinBox::up-button:pressed, QDoubleSpinBox::up-button:pressed,
QSpinBox::down-button:pressed, QDoubleSpinBox::down-button:pressed {
    background-color: __BUTTON_PRESSED__;
}
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {
    image: url(__SPIN_UP__);
    width: 8px;
    height: 8px;
}
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {
    image: url(__SPIN_DOWN__);
    width: 8px;
    height: 8px;
}

QPushButton {
    background-color: __BUTTON__;
    border: 1px solid __BORDER__;
    border-radius: 6px;
    padding: 4px 10px;
}
QPushButton:hover {
    background-color: __BUTTON_HOVER__;
}
QPushButton:pressed {
    background-color: __BUTTON_PRESSED__;
}
QPushButton:disabled {
    color: __MUTED__;
    background-color: __BUTTON__;
}

QToolButton {
    background-color: transparent;
    border: 1px solid transparent;
    border-radius: 6px;
    padding: 3px;
}
QToolButton:hover {
    background-color: __BUTTON_HOVER__;
    border-color: __BORDER__;
}
QToolButton:checked {
    background-color: __ACCENT_SOFT__;
    border-color: __ACCENT__;
}
QToolButton:checked:hover {
    background-color: __ACCENT_SOFT__;
    border-color: __ACCENT_HOVER__;
}

QCheckBox, QRadioButton {
    spacing: 6px;
}
QCheckBox::indicator, QRadioButton::indicator {
    width: 14px;
    height: 14px;
    border: 1px solid __INPUT_BORDER__;
    border-radius: 3px;
    background: __INPUT_BG__;
}
QCheckBox::indicator:checked {
    background: __ACCENT__;
    border: 1px solid __ACCENT__;
}
QRadioButton::indicator {
    border-radius: 7px;
}
QRadioButton::indicator:checked {
    background: __ACCENT__;
    border: 1px solid __ACCENT__;
}

QScrollBar:vertical {
    background: __SCROLL_TRACK__;
    width: 10px;
    margin: 2px;
    border-radius: 5px;
}
QScrollBar::handle:vertical {
    background: __SCROLL_THUMB__;
    min-height: 24px;
    border-radius: 4px;
}
QScrollBar::handle:vertical:hover {
    background: __SCROLL_THUMB_HOVER__;
}
QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical {
    height: 0px;
}

QScrollBar:horizontal {
    background: __SCROLL_TRACK__;
    height: 10px;
    margin: 2px;
    border-radius: 5px;
}
QScrollBar::handle:horizontal {
    background: __SCROLL_THUMB__;
    min-width: 24px;
    border-radius: 4px;
}
QScrollBar::handle:horizontal:hover {
    background: __SCROLL_THUMB_HOVER__;
}
QScrollBar::add-line:horizontal,
QScrollBar::sub-line:horizontal {
    width: 0px;
}

QHeaderView::section {
    background-color: __ALT_BASE__;
    color: __TEXT__;
    padding: 4px 6px;
    border: 1px solid __BORDER__;
}

QAbstractItemView::item:selected {
    background-color: __HIGHLIGHT__;
    color: __HIGHLIGHTED_TEXT__;
}
)");

    style.replace( "__WINDOW__", toCss( t.window ) );
    style.replace( "__TEXT__", toCss( t.text ) );
    style.replace( "__TOOLTIP_BG__", toCss( t.toolTipBase ) );
    style.replace( "__TOOLTIP_TEXT__", toCss( t.toolTipText ) );
    style.replace( "__BORDER__", toCss( t.border ) );
    style.replace( "__TOOLBAR_TOP__", toCss( t.toolbarGradientTop ) );
    style.replace( "__TOOLBAR_BOTTOM__", toCss( t.toolbarGradientBottom ) );
    style.replace( "__MENU_BG__", toCss( t.menuBg ) );
    style.replace( "__MENU_HOVER__", toCss( t.menuHover ) );
    style.replace( "__BASE__", toCss( t.base ) );
    style.replace( "__ALT_BASE__", toCss( t.alternateBase ) );
    style.replace( "__MUTED__", toCss( t.mutedText ) );
    style.replace( "__TAB_CLOSE__", closeButtonIcon );
    style.replace( "__TAB_CLOSE_HOVER__", closeButtonHoverIcon );
    style.replace( "__SPIN_UP__", spinArrowUp );
    style.replace( "__SPIN_DOWN__", spinArrowDown );
    style.replace( "__INPUT_BG__", toCss( t.inputBg ) );
    style.replace( "__INPUT_BORDER__", toCss( t.inputBorder ) );
    style.replace( "__INPUT_FOCUS__", toCss( t.inputFocusBorder ) );
    style.replace( "__BUTTON__", toCss( t.button ) );
    style.replace( "__BUTTON_HOVER__", toCss( t.buttonHover ) );
    style.replace( "__BUTTON_PRESSED__", toCss( t.buttonPressed ) );
    style.replace( "__ACCENT__", toCss( t.accent ) );
    style.replace( "__ACCENT_HOVER__", toCss( t.accentHover ) );
    style.replace( "__ACCENT_SOFT__", toCss( t.accentSoft ) );
    style.replace( "__SCROLL_TRACK__", toCss( t.scrollbarTrack ) );
    style.replace( "__SCROLL_THUMB__", toCss( t.scrollbarThumb ) );
    style.replace( "__SCROLL_THUMB_HOVER__", toCss( t.scrollbarThumbHover ) );
    style.replace( "__HIGHLIGHT__", toCss( t.highlight ) );
    style.replace( "__HIGHLIGHTED_TEXT__", toCss( t.highlightedText ) );

    return style;
}

static ThemeVariant resolveThemeVariant( ThemeMode mode )
{
    if ( mode == ThemeMode::Dark ) {
        return ThemeVariant::Dark;
    }
    if ( mode == ThemeMode::Light ) {
        return ThemeVariant::Light;
    }
    return isSystemDarkTheme() ? ThemeVariant::Dark : ThemeVariant::Light;
}

static QFont uiFont()
{
    QFont font = QFontDatabase::systemFont( QFontDatabase::GeneralFont );
    if ( font.pointSizeF() > 0.0 ) {
        font.setPointSizeF( font.pointSizeF() + 0.5 );
    }
    font.setStyleStrategy( QFont::PreferAntialias );
    return font;
}
} // namespace

QStringList StyleManager::availableStyles()
{
    return { ModernKey, SystemKey, DarkStyleKey };
}

QString StyleManager::defaultPlatformStyle()
{
#if defined( Q_OS_WIN )
    return VistaKey;
#elif defined( Q_OS_MACOS )
    return MacintoshKey;
#else
    return FusionKey;
#endif
}

QString StyleManager::defaultStyle()
{
    return ModernKey;
}

// Detect system theme preference
static bool isSystemDarkTheme()
{
#ifdef Q_OS_WIN
    // Windows 10/11 dark mode detection
    HKEY hKey;
    if ( RegOpenKeyExW( HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey ) == ERROR_SUCCESS ) {
        DWORD value = 0;
        DWORD size = sizeof( DWORD );
        if ( RegQueryValueExW( hKey, L"AppsUseLightTheme", nullptr, nullptr, reinterpret_cast<LPBYTE>( &value ), &size ) == ERROR_SUCCESS ) {
            RegCloseKey( hKey );
            return value == 0; // 0 means dark theme
        }
        RegCloseKey( hKey );
    }
    return false;
#elif defined( Q_OS_MACOS )
    // macOS dark mode detection
#if QT_VERSION >= QT_VERSION_CHECK( 6, 5, 0 )
    if ( QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark ) {
        return true;
    }
#endif
    QSettings globalSettings( "NSGlobalDomain", QSettings::NativeFormat );
    const auto style = globalSettings.value( "AppleInterfaceStyle", "Light" ).toString();
    if ( style.compare( "Dark", Qt::CaseInsensitive ) == 0 ) {
        return true;
    }
    QSettings appleSettings( "Apple Global Domain", QSettings::NativeFormat );
    const auto legacyStyle = appleSettings.value( "AppleInterfaceStyle", "Light" ).toString();
    return legacyStyle.compare( "Dark", Qt::CaseInsensitive ) == 0;
#else
    // Linux: Check GTK theme or environment variable
    QSettings gtkSettings( QSettings::UserScope, "gtk-3.0", "settings" );
    QString gtkTheme = gtkSettings.value( "gtk-theme-name", "" ).toString().toLower();
    if ( gtkTheme.contains( "dark" ) ) {
        return true;
    }
    // Check environment variable
    QString envTheme = qgetenv( "GTK_THEME" ).toLower();
    if ( envTheme.contains( "dark" ) ) {
        return true;
    }
    return false;
#endif
}

void StyleManager::applyStyle( const QString& style )
{
    static bool applyingStyle = false;
    if ( applyingStyle ) {
        return;
    }
    struct ApplyGuard {
        bool& flag;
        explicit ApplyGuard( bool& value )
            : flag( value )
        {
            flag = true;
        }
        ~ApplyGuard()
        {
            flag = false;
        }
    } guard( applyingStyle );

    ensureThemeWatcher();

    LOG_INFO << "Setting style to " << style;

    // Apply theme based on theme mode
    const auto& config = Configuration::get();
    if ( style == ModernKey ) {
        const auto variant = resolveThemeVariant( config.themeMode() );
        const auto tokens
            = ( variant == ThemeVariant::Dark ) ? modernDarkTokens() : modernLightTokens();

        qApp->setStyle( QStyleFactory::create( FusionKey ) );
        qApp->setPalette( paletteFromTokens( tokens ) );
        qApp->setStyleSheet( buildModernStyleSheet( tokens, variant == ThemeVariant::Dark ) );
        qApp->setFont( uiFont() );
        return;
    }

    const bool isSystemStyle = ( style == SystemKey );
    QString actualStyle = isSystemStyle ? defaultPlatformStyle() : style;
    const auto mode = config.themeMode();
    const bool systemDark = isSystemDarkTheme();

    if ( mode == ThemeMode::Auto ) {
        if ( systemDark ) {
#ifdef Q_OS_WIN
            actualStyle = DarkWindowsStyleKey;
#else
            actualStyle = DarkStyleKey;
#endif
        }
        else {
            actualStyle = defaultPlatformStyle();
        }
        LOG_INFO << "Auto theme mode: system is " << ( systemDark ? "dark" : "light" )
                 << ", using style " << actualStyle;
    }
    else if ( mode == ThemeMode::Dark ) {
#ifdef Q_OS_WIN
        actualStyle = DarkWindowsStyleKey;
#else
        actualStyle = DarkStyleKey;
#endif
        LOG_INFO << "Dark theme mode: forcing dark style " << actualStyle;
    }
    else {
        if ( style == DarkStyleKey || style == DarkWindowsStyleKey ) {
            actualStyle = defaultPlatformStyle();
            LOG_INFO << "Light theme mode: overriding dark style to " << actualStyle;
        }
        else if ( isSystemStyle ) {
            actualStyle = defaultPlatformStyle();
        }
        else {
            actualStyle = style;
        }
    }

    if ( actualStyle == DarkStyleKey || actualStyle == DarkWindowsStyleKey ) {
        const auto palette = Configuration::get().darkPalette();

        QPalette darkPalette;
        darkPalette.setColor( QPalette::Window, QColor( palette.at( "Window" ) ) );
        darkPalette.setColor( QPalette::WindowText, QColor( palette.at( "WindowText" ) ) );
        darkPalette.setColor( QPalette::Base, QColor( palette.at( "Base" ) ) );
        darkPalette.setColor( QPalette::AlternateBase, QColor( palette.at( "AlternateBase" ) ) );
        darkPalette.setColor( QPalette::ToolTipBase, QColor( palette.at( "ToolTipBase" ) ) );
        darkPalette.setColor( QPalette::ToolTipText, QColor( palette.at( "ToolTipText" ) ) );
        darkPalette.setColor( QPalette::Text, QColor( palette.at( "Text" ) ) );
        darkPalette.setColor( QPalette::Button, QColor( palette.at( "Button" ) ) );
        darkPalette.setColor( QPalette::ButtonText, QColor( palette.at( "ButtonText" ) ) );
        darkPalette.setColor( QPalette::Link, QColor( palette.at( "Link" ) ) );
        darkPalette.setColor( QPalette::Highlight, QColor( palette.at( "Highlight" ) ) );
        darkPalette.setColor( QPalette::HighlightedText,
                              QColor( palette.at( "HighlightedText" ) ) );

        darkPalette.setColor( QPalette::Active, QPalette::Button,
                              QColor( palette.at( "ActiveButton" ) ) );
        darkPalette.setColor( QPalette::Disabled, QPalette::ButtonText,
                              QColor( palette.at( "DisabledButtonText" ) ) );
        darkPalette.setColor( QPalette::Disabled, QPalette::WindowText,
                              QColor( palette.at( "DisabledWindowText" ) ) );
        darkPalette.setColor( QPalette::Disabled, QPalette::Text,
                              QColor( palette.at( "DisabledText" ) ) );
        darkPalette.setColor( QPalette::Disabled, QPalette::Light,
                              QColor( palette.at( "DisabledLight" ) ) );

        if ( actualStyle == DarkWindowsStyleKey ) {
            qApp->setStyle( QStyleFactory::create( WindowsKey ) );
        }
        else {
            qApp->setStyle( QStyleFactory::create( FusionKey ) );
        }

        qApp->setPalette( darkPalette );
        qApp->setStyleSheet( "" );
        qApp->setFont( QFontDatabase::systemFont( QFontDatabase::GeneralFont ) );
    }
    else {
        qApp->setStyle( actualStyle );
        qApp->setPalette( qApp->style()->standardPalette() );
        qApp->setStyleSheet( "" );
        qApp->setFont( QFontDatabase::systemFont( QFontDatabase::GeneralFont ) );
    }
}
