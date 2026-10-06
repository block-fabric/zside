// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/theme.h>

#include <QApplication>
#include <QColor>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QObject>
#include <QFontMetrics>
#include <QPalette>
#include <QScreen>
#include <QSize>
#include <QLayout>
#include <QTimer>
#include <QWidget>
#include <QEvent>
#include <QPushButton>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>

#include <algorithm>
#include <optional>

namespace Theme {
namespace {
const char* const SETTING{"Theme"};
const char* const SYSTEM{"system"};
const char* const SETTING_FONT_FAMILY{"FontFamily"};
const char* const SETTING_FONT_SIZE{"FontSizeAdjustment"};
const char* const SETTING_FRAME{"ThemedFrame"};

/** The colours a theme is made of. The rest of the palette is derived from them. */
struct Colors {
    const char* id;
    const char* name;
    //! Background of windows, and the text on it.
    QColor window, window_text;
    //! Background of lists and text fields, of every other row of a list, and the text on them.
    QColor base, alternate_base, text;
    QColor button, button_text;
    //! Selected items and other accents, and the text on them.
    QColor highlight, highlighted_text;
    QColor link;
    //! Resource with a complete style sheet, for a theme that restyles every kind of widget.
    const char* style_sheet{nullptr};
};

const Colors THEMES[]{
    // Complete style sheets, from the Skydoge wallet. The colours next to each are the
    // ones the icons and the widgets without a style of their own are drawn with.
    {"light", QT_TRANSLATE_NOOP("Theme", "Light"),
     "#f4f6f9", "#171a1f", "#f6f8fa", "#e8ebee", "#171a1f", "#d9dbde", "#171a1f", "#0aa46b", "#ffffff", "#0aa46b", ":/themes/light"},
    {"dark", QT_TRANSLATE_NOOP("Theme", "Dark"),
     "#14161a", "#e8eaed", "#0f1013", "#1e2024", "#e8eaed", "#2d2f33", "#e8eaed", "#4d9fff", "#051221", "#4d9fff", ":/themes/dark"},
    {"classic", QT_TRANSLATE_NOOP("Theme", "Classic"),
     "#eceae4", "#1c1c1a", "#f0efea", "#e1dfd9", "#1c1c1a", "#d3d1cb", "#1c1c1a", "#d6e4f2", "#10314f", "#d6e4f2", ":/themes/classic"},
    {"sepia", QT_TRANSLATE_NOOP("Theme", "Sepia Parchment"),
     "#efe6cf", "#4b3b2a", "#f3ecdb", "#e6ddc6", "#4b3b2a", "#dbd1bb", "#4b3b2a", "#8b5e34", "#fbf6ea", "#8b5e34", ":/themes/sepia"},
    {"rose", QT_TRANSLATE_NOOP("Theme", "Rose Quartz"),
     "#fff5f7", "#4a2c33", "#fff7f9", "#f5eaed", "#4a2c33", "#e9dcdf", "#4a2c33", "#e0637f", "#ffffff", "#e0637f", ":/themes/rose"},
    {"midnight", QT_TRANSLATE_NOOP("Theme", "Midnight Money"),
     "#0d1117", "#e6edf3", "#090c11", "#171c22", "#e6edf3", "#272b31", "#e6edf3", "#3d8bfd", "#e6edf3", "#3d8bfd", ":/themes/midnight"},
    {"ocean", QT_TRANSLATE_NOOP("Theme", "Ocean Teal"),
     "#06232e", "#d6f0f5", "#041a22", "#102d37", "#d6f0f5", "#1e3b45", "#d6f0f5", "#3d8bfd", "#d6f0f5", "#3d8bfd", ":/themes/ocean"},
    {"nord", QT_TRANSLATE_NOOP("Theme", "Nord Frost"),
     "#2e3440", "#eceff4", "#222730", "#373d49", "#eceff4", "#444a55", "#eceff4", "#3d8bfd", "#eceff4", "#3d8bfd", ":/themes/nord"},
    {"dracula", QT_TRANSLATE_NOOP("Theme", "Dracula"),
     "#21222c", "#f8f8f2", "#181921", "#2b2c35", "#f8f8f2", "#3a3b43", "#f8f8f2", "#3d8bfd", "#f8f8f2", "#3d8bfd", ":/themes/dracula"},
    {"glass", QT_TRANSLATE_NOOP("Theme", "Glass Neobank"),
     "#0e1220", "#eaf0ff", "#0a0d18", "#191d2b", "#eaf0ff", "#282c3a", "#eaf0ff", "#3d8bfd", "#eaf0ff", "#3d8bfd", ":/themes/glass"},
    {"amethyst", QT_TRANSLATE_NOOP("Theme", "Amethyst Royal"),
     "#1a1327", "#ede9f5", "#130e1d", "#241d31", "#ede9f5", "#332c3f", "#ede9f5", "#3d8bfd", "#ede9f5", "#3d8bfd", ":/themes/amethyst"},
    {"wow", QT_TRANSLATE_NOOP("Theme", "Wow"),
     "#0e0b1c", "#f2eefb", "#0a0815", "#191627", "#f2eefb", "#292636", "#f2eefb", "#7c3aed", "#ffffff", "#7c3aed", ":/themes/wow"},
    {"solar", QT_TRANSLATE_NOOP("Theme", "Solar Gold"),
     "#131110", "#f5efe6", "#0e0c0c", "#1e1c1a", "#f5efe6", "#2e2b29", "#f5efe6", "#3d8bfd", "#f5efe6", "#3d8bfd", ":/themes/solar"},
    {"crimson", QT_TRANSLATE_NOOP("Theme", "Crimson Ember"),
     "#1a1213", "#f6e9e8", "#130d0e", "#251c1d", "#f6e9e8", "#342b2c", "#f6e9e8", "#3d8bfd", "#f6e9e8", "#3d8bfd", ":/themes/crimson"},
    {"neon", QT_TRANSLATE_NOOP("Theme", "Neon Terminal"),
     "#070b11", "#cfeaf2", "#05080c", "#11161c", "#cfeaf2", "#1f252c", "#cfeaf2", "#00e5ff", "#04161a", "#00e5ff", ":/themes/neon"},
    {"matrix", QT_TRANSLATE_NOOP("Theme", "Matrix Green"),
     "#000000", "#33ff66", "#000000", "#020c05", "#33ff66", "#061e0c", "#33ff66", "#3d8bfd", "#00ff41", "#3d8bfd", ":/themes/matrix"},
    // The dark theme of the Drivechain wallets, based on QDarkStyleSheet.
    {"qdarkstyle", QT_TRANSLATE_NOOP("Theme", "Dark (classic Drivechain)"),
     "#19232d", "#f0f0f0", "#19232d", "#1c2a36", "#f0f0f0", "#32414b", "#f0f0f0", "#1464a0", "#f0f0f0", "#148cd2", ":/themes/qdarkstyle"},
};

const Colors* Find(const QString& id)
{
    for (const Colors& theme : THEMES) {
        if (id == theme.id) return &theme;
    }
    return nullptr;
}

/** A colour between two others. */
QColor Mix(const QColor& a, const QColor& b, double share_of_b)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * share_of_b,
                            a.greenF() + (b.greenF() - a.greenF()) * share_of_b,
                            a.blueF() + (b.blueF() - a.blueF()) * share_of_b);
}

QPalette MakePalette(const Colors& c)
{
    QPalette palette;
    palette.setColor(QPalette::Window, c.window);
    palette.setColor(QPalette::WindowText, c.window_text);
    palette.setColor(QPalette::Base, c.base);
    palette.setColor(QPalette::AlternateBase, c.alternate_base);
    palette.setColor(QPalette::Text, c.text);
    palette.setColor(QPalette::Button, c.button);
    palette.setColor(QPalette::ButtonText, c.button_text);
    palette.setColor(QPalette::BrightText, Qt::red);
    palette.setColor(QPalette::Highlight, c.highlight);
    palette.setColor(QPalette::HighlightedText, c.highlighted_text);
    palette.setColor(QPalette::Link, c.link);
    palette.setColor(QPalette::LinkVisited, Mix(c.link, c.window_text, 0.35));
    palette.setColor(QPalette::ToolTipBase, c.button);
    palette.setColor(QPalette::ToolTipText, c.button_text);
    palette.setColor(QPalette::PlaceholderText, Mix(c.text, c.base, 0.55));
    // The shades that frames and bevels are drawn with.
    palette.setColor(QPalette::Light, Mix(c.button, c.window_text, 0.18));
    palette.setColor(QPalette::Midlight, Mix(c.button, c.window_text, 0.09));
    palette.setColor(QPalette::Mid, Mix(c.window, c.window_text, 0.22));
    palette.setColor(QPalette::Dark, Mix(c.window, c.window_text, 0.32));
    palette.setColor(QPalette::Shadow, Mix(c.window, Qt::black, 0.6));

    const QColor disabled{Mix(c.window_text, c.window, 0.55)};
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        palette.setColor(QPalette::Disabled, role, disabled);
    }
    palette.setColor(QPalette::Disabled, QPalette::Highlight, Mix(c.highlight, c.window, 0.6));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
    return palette;
}

/** A colour as a faint wash over what is behind it, in the notation of style sheets. */
QString Tint(const QColor& color)
{
    return QStringLiteral("rgba(%1, %2, %3, 56)").arg(color.red()).arg(color.green()).arg(color.blue());
}

QString MakeStyleSheet(const Colors& c)
{
    return QStringLiteral(
               "QToolTip { color: %1; background-color: %2; border: 1px solid %3; }"
               "QToolBar { border: none; }"
               "QToolBar#sidebar { border-right: 2px solid %3; background-color: %2; spacing: 1px; padding: 4px 0px; }"
               "QPushButton#sidebarEntry { background: transparent; border: none; border-radius: 4px; padding: 5px 8px 5px 6px; margin: 0px 3px; text-align: left; font-weight: normal; }"
               "QPushButton#sidebarEntry:hover { background-color: rgba(128, 128, 128, 45); }"
               "QPushButton#sidebarEntry:checked { background-color: %5; color: %1; border-left: 3px solid %3; padding-left: 3px; font-weight: bold; }"
               "QToolBar#sidebar QToolButton#textSmaller, QToolBar#sidebar QToolButton#textLarger { padding: 2px 3px; }"
               "QToolBar QToolButton:checked { background-color: %3; color: %4; border-radius: 4px; }"
               "QGroupBox { font-weight: bold; }"
               "QHeaderView::section { background-color: %2; color: %1; padding: 4px; border: none; border-bottom: 1px solid %3; }")
        .arg(c.button_text.name(), c.button.name(), c.highlight.name(), c.highlighted_text.name(), Tint(c.highlight));
}

QString SidebarStyleSheet(const Colors& c)
{
    return QStringLiteral(
               "QToolBar#sidebar { border: none; border-right: 2px solid %1; spacing: 1px; padding: 4px 0px; min-width: 0px; }"
               "QPushButton#sidebarEntry { background: transparent; border: none; border-radius: 4px; padding: 5px 8px 5px 6px; margin: 0px 3px; min-width: 0px; text-align: left; font-weight: normal; }"
               "QPushButton#sidebarEntry:hover { background-color: rgba(128, 128, 128, 45); }"
               "QPushButton#sidebarEntry:checked { background-color: %2; color: %3; border-left: 3px solid %1; padding-left: 3px; font-weight: bold; }"
               "QPushButton#sidebarEntry:disabled { background: transparent; }"
               "QToolBar#sidebar QToolButton#textSmaller, QToolBar#sidebar QToolButton#textLarger { padding: 2px 3px; }"
               "QWidget#sidebarMining QPushButton { padding: 4px 6px; min-width: 0px; }"
               "QWidget#sidebarMining QPushButton:checked { background-color: %2; border: 1px solid %1; color: %3; }"
               "QWidget#sidebarMining QLineEdit { padding: 3px 4px; min-width: 0px; }")
        .arg(c.highlight.name(), Tint(c.highlight), c.window_text.name());
}

/** The title bar and the border of a window framed by ThemedFrame. */
QString FrameStyleSheet(const Colors& c)
{
    return QStringLiteral(
               "QWidget#titleBar { background-color: %1; border-bottom: 1px solid %3; }"
               "QWidget#titleBar QMenuBar { background-color: transparent; }"
               "QLabel#titleText { color: %2; font-weight: bold; background-color: transparent; }"
               "QLabel#titleIcon { background-color: transparent; }"
               "QToolButton#titleButton, QToolButton#titleClose { border: none; border-radius: 0px; padding: 5px 14px; color: %2; background-color: transparent; }"
               "QToolButton#titleButton:hover { background-color: %3; color: %4; }"
               "QToolButton#titleClose:hover { background-color: #d9343f; color: #ffffff; }"
               // The border the window is resized by is there, and does not show.
               "QFrame#frameEdge { background-color: %5; border: none; }")
        .arg(c.button.name(), c.button_text.name(), c.highlight.name(), c.highlighted_text.name(), c.window.name());
}

/** Kinds of font a look asks for. Which family it gets depends on what the computer has. */
enum class FontKind { DESKTOP, SANS, ROUNDED, SERIF, MONO };

struct LookDef {
    //! The theme whose colours the look has; the look goes by its name too.
    const char* theme;
    FontKind font;
};

const LookDef LOOKS[]{
    {"system", FontKind::DESKTOP},
    {"light", FontKind::SANS},
    {"dark", FontKind::SANS},
    {"classic", FontKind::SERIF},
    {"sepia", FontKind::SERIF},
    {"rose", FontKind::ROUNDED},
    {"midnight", FontKind::SANS},
    {"ocean", FontKind::ROUNDED},
    {"nord", FontKind::SANS},
    {"dracula", FontKind::MONO},
    {"glass", FontKind::ROUNDED},
    {"amethyst", FontKind::SERIF},
    {"wow", FontKind::ROUNDED},
    {"solar", FontKind::SERIF},
    {"crimson", FontKind::SANS},
    {"neon", FontKind::MONO},
    {"matrix", FontKind::MONO},
    {"qdarkstyle", FontKind::DESKTOP},
};

/** The first family of its kind that this computer has; empty, the font of the desktop, if it has none. */
QString FontFamily(FontKind kind)
{
    QStringList wanted;
    switch (kind) {
    case FontKind::DESKTOP: return {};
    case FontKind::SANS: wanted = {"Inter", "Segoe UI", "Noto Sans", "Open Sans", "DejaVu Sans", "Helvetica Neue", "Arial"}; break;
    case FontKind::ROUNDED: wanted = {"Nunito", "Quicksand", "Ubuntu", "Cantarell", "Verdana", "Trebuchet MS", "DejaVu Sans"}; break;
    case FontKind::SERIF: wanted = {"Georgia", "Noto Serif", "DejaVu Serif", "Liberation Serif", "Times New Roman"}; break;
    case FontKind::MONO: wanted = {"JetBrains Mono", "Fira Code", "Cascadia Mono", "DejaVu Sans Mono", "Menlo", "Consolas", "Liberation Mono", "Courier New"}; break;
    }
    static const QStringList installed{QFontDatabase::families()};
    for (const QString& family : wanted) {
        if (installed.contains(family)) return family;
    }
    return {};
}

/**
 * Keeps push buttons at least as wide as their text. A style sheet that gives buttons a minimum width
 * makes that the least a layout may give them, and a crowded row then cuts their text off, the more so
 * the larger the text is.
 *
 * And keeps windows at least as large as what is in them: a window that was given a size when it was
 * made, for the text of the desktop, is too small for larger text, and what is in it gets squeezed.
 */
class ButtonFitter : public QObject
{
public:
    using QObject::QObject;

    static void Fit(QPushButton* button)
    {
        if (button->text().isEmpty()) return;
        QString text{button->text()};
        text.remove(QLatin1Char('&'));
        // The themes write buttons in bold, which the metrics of the button do not always know yet.
        QFont bold{button->font()};
        bold.setBold(true);
        int width{QFontMetrics{bold}.horizontalAdvance(text) + 36};
        if (!button->icon().isNull()) width += button->iconSize().width() + 6;
        if (button->minimumWidth() != width) button->setMinimumWidth(width);
    }

    static void Fit(QWidget* window)
    {
        if (!window->isWindow() || !window->isVisible() || !window->layout() || window->isMaximized() || window->isFullScreen()) return;
        window->layout()->activate();
        // The size the contents would like, not the least they put up with: under a style sheet that is
        // less than what they need. No larger than the screen has room for.
        QSize needed{window->layout()->totalSizeHint()};
        if (const QScreen* screen{window->screen()}) needed = needed.boundedTo(screen->availableGeometry().size() - QSize(40, 60));
        if (needed.width() > window->width() || needed.height() > window->height()) window->resize(window->size().expandedTo(needed));
    }

protected:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        switch (event->type()) {
        case QEvent::Show:
        case QEvent::FontChange:
        case QEvent::StyleChange:
        case QEvent::LanguageChange:
            if (auto* button{qobject_cast<QPushButton*>(object)}) {
                Fit(button);
            } else if (auto* widget{qobject_cast<QWidget*>(object)}; widget && widget->isWindow() && event->type() == QEvent::Show) {
                // Once what is in the window has its fonts and its sizes.
                QTimer::singleShot(0, widget, [widget] { Fit(widget); });
            }
            break;
        default:
            break;
        }
        return false;
    }
};

/** The look the application had before a theme was applied. */
struct Original {
    QPalette palette;
    QString style;
    QFont font;
};
std::optional<Original> g_original;
} // namespace

Signals* Changes()
{
    static Signals signals_object;
    return &signals_object;
}

QList<Info> Looks()
{
    QList<Info> looks;
    for (const LookDef& look : LOOKS) {
        const Colors* theme{Find(look.theme)};
        QString name{theme ? QApplication::translate("Theme", theme->name) : QObject::tr("Same as the desktop")};
        if (const QString family{FontFamily(look.font)}; !family.isEmpty()) name += QStringLiteral(" · ") + family;
        looks.push_back({look.theme, name});
    }
    return looks;
}

QString SavedLook()
{
    for (const LookDef& look : LOOKS) {
        // The size of the text is the user's own business, whatever the look.
        if (Saved() == look.theme && SavedFontFamily() == FontFamily(look.font)) return look.theme;
    }
    return {};
}

void SaveLook(const QString& id)
{
    for (const LookDef& look : LOOKS) {
        if (id != look.theme) continue;
        QSettings settings;
        settings.setValue(SETTING, id);
        settings.setValue(SETTING_FONT_FAMILY, FontFamily(look.font));
        Apply(id);
        Q_EMIT Changes()->changed();
        return;
    }
}

QList<Info> Available()
{
    QList<Info> themes{{SYSTEM, QObject::tr("Same as the desktop")}};
    for (const Colors& theme : THEMES) themes.push_back({theme.id, QApplication::translate("Theme", theme.name)});
    return themes;
}

QString Saved()
{
    const QString id{QSettings().value(SETTING, SYSTEM).toString()};
    return Find(id) ? id : QString{SYSTEM};
}

void Save(const QString& id)
{
    QSettings().setValue(SETTING, id);
    Apply(id);
    Q_EMIT Changes()->changed();
}

void Apply(const QString& id)
{
    const Colors* theme{Find(id)};
    if (!g_original) {
        if (!theme) {
            ApplyFont();
            return;
        }
        g_original = Original{QApplication::palette(), QApplication::style()->objectName(), QApplication::font()};
    }
    if (theme) {
        // The Fusion style draws everything with the palette, which the styles of some desktops do not.
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        QApplication::setPalette(MakePalette(*theme));
        QString style_sheet{MakeStyleSheet(*theme) + FrameStyleSheet(*theme)};
        if (theme->style_sheet) {
            QFile file{theme->style_sheet};
            // The sheet of the theme, then what this wallet has that the sheet does not know of.
            if (file.open(QFile::ReadOnly | QFile::Text)) style_sheet = QString::fromUtf8(file.readAll()) + SidebarStyleSheet(*theme) + FrameStyleSheet(*theme);
        }
        qApp->setStyleSheet(style_sheet);
    } else {
        qApp->setStyleSheet({});
        QApplication::setStyle(QStyleFactory::create(g_original->style));
        QApplication::setPalette(g_original->palette);
    }
    ApplyFont();
}

bool SavedFrame() { return QSettings().value(SETTING_FRAME, true).toBool(); }

void SaveFrame(bool themed)
{
    QSettings().setValue(SETTING_FRAME, themed);
    Q_EMIT Changes()->changed();
}

bool FrameWanted()
{
#ifdef Q_OS_MACOS
    // The menus are at the top of the screen there, and the frame is the one of the system.
    return false;
#else
    return SavedFrame() && Find(Saved()) != nullptr;
#endif
}

QString SavedFontFamily() { return QSettings().value(SETTING_FONT_FAMILY).toString(); }

int SavedFontSizeAdjustment() { return std::clamp(QSettings().value(SETTING_FONT_SIZE, DEFAULT_FONT_SIZE_ADJUSTMENT).toInt(), MIN_FONT_SIZE_ADJUSTMENT, MAX_FONT_SIZE_ADJUSTMENT); }

void SaveFont(const QString& family, int size_adjustment)
{
    QSettings settings;
    settings.setValue(SETTING_FONT_FAMILY, family);
    settings.setValue(SETTING_FONT_SIZE, std::clamp(size_adjustment, MIN_FONT_SIZE_ADJUSTMENT, MAX_FONT_SIZE_ADJUSTMENT));
    ApplyFont();
    Q_EMIT Changes()->changed();
}

void ApplyFont()
{
    static ButtonFitter* fitter{nullptr};
    if (!fitter) {
        fitter = new ButtonFitter(qApp);
        qApp->installEventFilter(fitter);
    }
    static const QFont original{QApplication::font()};
    const QString family{SavedFontFamily()};
    const int adjustment{SavedFontSizeAdjustment()};
    QFont font{original};
    if (!family.isEmpty()) font.setFamily(family);
    if (font.pointSizeF() > 0) font.setPointSizeF(std::max(4.0, font.pointSizeF() + adjustment));
    QApplication::setFont(font);
    // Widgets styled by a sheet take their font from it, so the sheet is told too.
    QString sheet{qApp->styleSheet()};
    static const QString MARK{QStringLiteral("/*font*/")};
    const int at{static_cast<int>(sheet.indexOf(MARK))};
    if (at >= 0) sheet.truncate(at);
    if (!family.isEmpty() || adjustment != 0) {
        sheet += MARK + QStringLiteral("QWidget { %1 font-size: %2pt; }")
                            .arg(family.isEmpty() ? QString{} : QStringLiteral("font-family: \"%1\";").arg(family))
                            .arg(font.pointSizeF() > 0 ? font.pointSizeF() : 9.0, 0, 'f', 1);
    }
    if (sheet != qApp->styleSheet()) qApp->setStyleSheet(sheet);
    // The buttons there are take the width their text has now.
    for (QWidget* widget : QApplication::allWidgets()) {
        if (auto* button{qobject_cast<QPushButton*>(widget)}) ButtonFitter::Fit(button);
    }
    for (QWidget* window : QApplication::topLevelWidgets()) {
        if (window->isVisible()) QTimer::singleShot(0, window, [window] { ButtonFitter::Fit(window); });
    }
}

} // namespace Theme
