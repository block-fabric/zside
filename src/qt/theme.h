// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_THEME_H
#define BITCOIN_QT_THEME_H

#include <QList>
#include <QObject>
#include <QString>

/** Colour themes of the GUI. */
namespace Theme {
struct Info {
    //! Name the theme is saved under in the settings.
    QString id;
    //! Name shown to the user.
    QString name;
};

/** Tells when the theme or the font changed. */
class Signals : public QObject
{
    Q_OBJECT

Q_SIGNALS:
    void changed();
};
Signals* Changes();

/** The themes, starting with the one that follows the desktop. */
QList<Info> Available();

/** The theme saved in the settings. */
QString Saved();

/** Save `id` as the theme to use and apply it. */
void Save(const QString& id);

/** Give the application the look of the theme `id`; an unknown one means the look of the desktop. */
void Apply(const QString& id);
inline constexpr int MIN_FONT_SIZE_ADJUSTMENT{-4};
inline constexpr int MAX_FONT_SIZE_ADJUSTMENT{12};
//! The text is a little larger than on the desktop unless the user says otherwise: the windows are dense.
inline constexpr int DEFAULT_FONT_SIZE_ADJUSTMENT{2};

/** The font family saved in the settings; empty for the one of the desktop. */
QString SavedFontFamily();
/** How many points larger (or smaller, if negative) than on the desktop the text is. */
int SavedFontSizeAdjustment();
/** Save the font settings and apply them. */
void SaveFont(const QString& family, int size_adjustment);
/** Give the application the font of the settings. */
void ApplyFont();

/** Whether the user wants windows framed in the colours of the theme, where there is a theme. */
bool SavedFrame();
void SaveFrame(bool themed);
/** Whether a window is to be framed in the colours of the theme now: the user wants it, and the look is not the one of the desktop. */
bool FrameWanted();

/**
 * The looks: each is the colours of a theme with a font that suits them. The
 * size of the text is set apart from the look. The first one is the look of the desktop.
 */
QList<Info> Looks();
/** The look the settings amount to; empty if they are a combination of the user's own. */
QString SavedLook();
/** Save the theme and the font of the look `id` and apply them. */
void SaveLook(const QString& id);
} // namespace Theme

#endif // BITCOIN_QT_THEME_H
