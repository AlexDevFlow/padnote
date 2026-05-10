// Localization.h — load nativeLang XML translations and apply them to
// the live menu bar.
//
// The nativeLang XML files (bundled at PowerEditor/installer/nativeLang/)
// map menu IDs (numeric command IDs) to translated display strings.
// padnote uses Qt tr() with English source strings; this module
// overlays the bundled translations on top of the live menu bar after
// construction, keyed off a `nlMenuId` QMenu property we set in
// MainWindow::buildMenus.
//
// Phase 7d MVP shipped top-level menu names only.
//
// Phase 8b — full menu coverage:
//   • Sub-menu titles via property tagging (`nlSubMenuId` on each QMenu).
//   • Leaf actions via SOURCE-TEXT MATCHING — at startup we parse
//     english.xml's <Commands> into a {normalised English text → numeric
//     IDM number} map, then for each leaf action we look up its current
//     English text and translate via the active language's
//     {IDM number → translated text} map. Zero per-action property
//     tagging required.
//   • applyToMenuBar() recurses through the entire tree.
//
// Phase 8b-polish — dialog translation framework. Same source-text
// matching pattern as 8b's menu translator, scoped per-dialog: at
// init() we parse english.xml's `<Dialog>/<DialogKey>/<Item id="N"
// name="text"/>` rows into a {dialogKey → {normalised English text →
// id}} index. setActive() refills a complementary {dialogKey → {id →
// translated text}} map from the active language XML. applyToDialog
// walks the dialog's QLabel/QCheckBox/QPushButton/QRadioButton/
// QGroupBox children via findChildren and translates by source-text
// lookup. Window-title translation: applyToDialog also reads the
// dialogKey's `title` attribute and sets the dialog's windowTitle.
// Currently wired in HashDialog + FindReplaceDialog as a proof; other
// dialogs adopt by adding a one-line applyToDialog call in their ctor.

#pragma once

#include <QHash>
#include <QString>
#include <QVector>

class QMenuBar;
class QMenu;
class QWidget;

namespace Localization {

struct Lang {
    QString displayName;     // value of <Native-Langue name="..."> (e.g. "Italiano")
    QString resourcePath;    // ":/nativeLang/<file>.xml"
};

// Build the registry of bundled languages by parsing each XML's
// <Native-Langue> header. Idempotent; safe to call multiple times.
// Pulls Config::uiLanguage() to set the active language at startup.
void init();

// Sorted by displayName. The "English" entry's resource is the upstream
// english.xml — selecting it re-overlays English strings (useful when the
// user's source-string defaults have drifted).
QVector<Lang> available();

// Active language displayName, or "" when no overlay is active (Qt
// tr() defaults shine through).
QString active();

// Switch the active language. Empty string clears the overlay. Persists
// to Config; caller is expected to invoke applyToMenuBar afterwards.
void setActive(const QString& displayName);

// Translation lookup by upstream menuId/subMenuId attribute. Returns
// "" if the active language has no entry — caller falls back to the
// QAction's existing text.
QString translate(const QString& menuId);

// Phase 8b — sub-menu title translation by `nlSubMenuId` (e.g.
// "file-recentFiles", "edit-insert"). Empty if not in the active
// language's <SubEntries>.
QString translateSubMenu(const QString& subMenuId);

// Phase 8b — leaf-action translation via source-text matching.
// Pass the action's normalised English source (from
// `Localization::normaliseEnglish`); returns the translated text or
// "" if either the English isn't in english.xml or the active language
// has no entry for the corresponding command ID.
QString translateCommandByEnglish(const QString& english);

// Phase 8b — normalise a Qt action/menu text for source-text lookup:
// strip leading mnemonic '&', strip trailing '...' / '…', drop the
// remaining '&' chars (Qt mnemonics elsewhere in the string), trim.
QString normaliseEnglish(const QString& s);

// Phase 8b — re-apply by walking the menu tree recursively.
// Top-level menus translate via `nlMenuId`; sub-menus via `nlSubMenuId`;
// leaf actions via source-text matching. The first apply records each
// action's original English text in the dynamic property
// `nlOriginalEnglish` so subsequent language switches translate from
// the snapshot rather than from currently-displayed (already-translated)
// text. Idempotent.
void applyToMenuBar(QMenuBar* mb);

// Phase 8b-polish — apply the active language overlay to a dialog
// or dock. `dialogKey` matches a child of `<Dialog>` in nativeLang
// XMLs (e.g. "Find", "Preference", "StyleConfig"). The widget can
// be a QDialog OR a QDockWidget — both have a windowTitle that
// matches the dialogKey's `title` attribute, and both host
// translatable QLabel/QCheckBox/etc. children. On first apply,
// snapshots each translatable widget's English text into
// `nlOriginalEnglish` so subsequent language switches translate
// from the snapshot. Pass nullptr or an empty key to no-op.
// Idempotent.
void applyToDialog(QWidget* widget, const char* dialogKey);

// Phase 8b-polish — dialog window title lookup. Reads the
// `<DialogKey title="..."/>` attribute from the active language XML.
// Returns "" when not in the active language (caller falls back to
// the dialog's existing windowTitle).
QString translateDialogTitle(const char* dialogKey);

} // namespace Localization
