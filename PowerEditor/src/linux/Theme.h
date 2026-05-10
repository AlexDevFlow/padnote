// Theme.h — XML-driven theme with Light/Dark switch.
//
// At init time we parse two theme XMLs embedded as Qt resources:
//   - stylers.model.xml    → default Light theme
//   - DarkModeDefault.xml  → default Dark theme
//
// Both share the same schema (LexerStyles / LexerType / WordsStyle,
// plus GlobalStyles / WidgetStyle). applyForLanguage dispatches on
// language internalName ("cpp", "python", ...) rather than Lexilla
// lexer ID, because the styler XML scopes styles per-language even
// when several languages share a Lexilla lexer (e.g. "c", "cpp",
// "java", "javascript.js" all use lexilla "cpp" but have different
// LexerType blocks).

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

class ScintillaEditBase;

namespace Theme {

enum class Mode { Light, Dark };

// Phase 8a — one row of per-language style data, returned by
// stylesForLanguage(). The Style Configurator renders one form row per
// entry. Values reflect "what the editor would currently render with"
// — theme defaults + user overrides merged. Setting an override on
// the styleID and asking again returns the user value, not the
// theme's.
struct LanguageStyleInfo {
    int     styleID;        // Scintilla style slot (e.g. SCE_C_COMMENT)
    QString name;           // stylers.xml name=, e.g. "COMMENT"
    int     fg;             // Scintilla packed colour, -1 = theme didn't set
    int     bg;             // Scintilla packed colour, -1 = theme didn't set
    int     fontStyle;      // bitmask: 1=bold 2=italic 4=underline
    int     fontSize;       // 0 = use STYLE_DEFAULT's size
    QString fontName;       // Phase 8a-polish — empty = use STYLE_DEFAULT's font
};

// Parse the embedded theme XMLs. Idempotent. main_qt.cpp calls this once
// after Languages::init().
void init();

// Currently active mode (light = the active theme is light, dark = dark).
// Defaults to Light until setMode() / setThemeByName() is called.
Mode mode();
void setMode(Mode m);

// Phase 7f — theme picker. Returns every available theme's display name,
// sorted (Default Light + Default Dark first, then bundled themes
// alphabetically). The current theme's name is exposed via currentName().
QStringList availableThemes();
QString     currentName();
bool        setThemeByName(const QString& name);  // false if name unknown

// Apply font, font size, line-number margin colours, and other GlobalStyles
// to a fresh editor. Called from Buffer's ctor and on theme switch.
void applyEditorBaseStyles(ScintillaEditBase* editor);

// Apply per-language SCE_* style colours for the given internal
// language name ("cpp", "python", "json", ...). No-op if the name is null,
// "normal", or not in the theme's lookup — the editor still highlights
// correctly, just in default colours.
void applyForLanguage(ScintillaEditBase* editor, const char* internalName);

// Reset all styles back to STYLE_DEFAULT.
void clearStyles(ScintillaEditBase* editor);

// Phase 7e — return the bookmark margin marker's fore/back colours
// (Scintilla packed format) from the active theme. Falls back to a
// hardcoded red if the theme doesn't define them. Buffer's ctor
// + reapplyTheme call this to colour the bookmark marker.
int bookmarkMarkerFore();
int bookmarkMarkerBack();

// Phase 9e — fold-margin marker colours. Reads "Fold margin" /
// "Fold" globals from stylers.xml; falls back to a theme-aware
// grey when unset. Used for the seven SC_MARKNUM_FOLDER* slots.
int foldMarkerFore();
int foldMarkerBack();

// Phase 9m — Document Map viewport-rectangle indicator colour.
// Derived from theme's "Current line background colour" (the
// in-buffer "you are here" affordance) so the map's "you are here"
// indicator visually matches. Falls back to a translucent grey
// when the theme doesn't define it.
int documentMapIndicatorFore();

// Phase 9m — Smart-highlight colour. Reads the theme's
// "Smart Highlighting" global. Used for both the in-text
// indicator (slot 9, INDIC_ROUNDBOX) and the new margin marker
// (Phase 9m.2). Falls back to BGR yellow if unset.
int smartHighlightFore();

// Phase 5MK — Mark Style 1..5 colours from stylers.xml. Each mark
// keyword pattern in Find/Replace's Mark tab gets its own indicator
// slot, coloured from these. `n` is 1..5; out-of-range returns a
// hardcoded fallback (BGR cyan/orange/yellow/purple/green per
// stylers.model.xml's defaults).
int markStyleFore(int n);

// Phase 8a — per-language Style Configurator support. `stylesForLanguage`
// returns one LanguageStyleInfo per `<WordsStyle>` in the active theme's
// `<LexerType name="<internalName>">` block, with user overrides merged
// in. Returns empty if the language has no theme entry.
//
// `languagesWithStyles` returns every internalName that has a non-empty
// styles list in the active theme, paired with its display name.
// Sorted by display name.
QVector<LanguageStyleInfo> stylesForLanguage(const char* internalName);

struct LanguageWithStyles {
    QString internalName;   // e.g. "cpp", "user", "udl:MyLang" — utf8 string identity
    QString displayName;    // human label (from langs.model.xml or stylers.xml desc)
};
QVector<LanguageWithStyles> languagesWithStyles();

} // namespace Theme
