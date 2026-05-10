#include "Localization.h"

#include <QCheckBox>
#include <QFile>
#include <QGroupBox>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QRadioButton>
#include <QWidget>
#include <algorithm>
#include <cstring>

#include "pugixml.hpp"

#include "Config.h"

namespace {

struct BundledEntry {
    const char* fileLeaf;     // for diagnostics / future config keying
    const char* resource;
};

// Curated subset of upstream's installer/nativeLang/. ~1.2 MB embedded.
// Adding a new language: drop a <file alias="nativeLang/<name>.xml">
// entry into resources.qrc and add a row here. The displayName comes
// from the XML's <Native-Langue name="..."> at runtime — no extra
// metadata needed in this table.
constexpr BundledEntry kBundled[] = {
    {"english.xml",           ":/nativeLang/english.xml"},
    {"french.xml",            ":/nativeLang/french.xml"},
    {"german.xml",            ":/nativeLang/german.xml"},
    {"italian.xml",           ":/nativeLang/italian.xml"},
    {"spanish.xml",           ":/nativeLang/spanish.xml"},
    {"portuguese.xml",        ":/nativeLang/portuguese.xml"},
    {"japanese.xml",          ":/nativeLang/japanese.xml"},
    {"chineseSimplified.xml", ":/nativeLang/chineseSimplified.xml"},
    {"russian.xml",           ":/nativeLang/russian.xml"},
    {"polish.xml",            ":/nativeLang/polish.xml"},
};

// Resource path for english.xml — used to build the source-text index
// at startup regardless of the active language.
constexpr const char* kEnglishResource = ":/nativeLang/english.xml";

struct State {
    QVector<Localization::Lang> registry;     // sorted by displayName
    QString                     activeDisplay; // "" = no overlay

    // Active-language translation maps. Refilled on every setActive().
    QHash<QString, QString>     idToText;          // top-level: menuId → translated
    QHash<QString, QString>     idToSubMenuText;   // sub-menu: subMenuId → translated
    QHash<QString, QString>     idToCommandText;   // leaf: numeric IDM string → translated

    // Phase 8b — built once at init() from english.xml, never refilled.
    // Maps normalised English (from <Commands>) → numeric IDM string.
    QHash<QString, QString>     englishTextToCommandId;

    // Phase 8b-polish — dialog translation state.
    // englishDialogTextToId: built once at init() from english.xml's
    //   <Dialog>/<DialogKey>/<Item id="N" name="text"/> rows.
    //   key1 = dialogKey (e.g. "Find"), key2 = normalised English text,
    //   value = string-form id (e.g. "1604" for "Match case").
    // idToDialogText: refilled on every setActive(). Same shape but
    //   keyed by dialogKey + id, value = translated text.
    // dialogKeyToTitle: refilled on every setActive(). Per-key
    //   `<DialogKey title="..."/>` attribute (the window title).
    QHash<QString, QHash<QString, QString>> englishDialogTextToId;
    QHash<QString, QHash<QString, QString>> idToDialogText;
    QHash<QString, QString>                 dialogKeyToTitle;

    bool                        loaded = false;
};

State& st()
{
    static State s;
    return s;
}

// Parse the <Native-Langue name="..."> from a resource and return the
// display name, or "" if parsing fails.
QString readDisplayName(const QString& resource)
{
    QFile f(resource);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = f.readAll();
    f.close();

    pugi::xml_document doc;
    const pugi::xml_parse_result r = doc.load_buffer(bytes.constData(),
        static_cast<size_t>(bytes.size()));
    if (!r) return {};

    pugi::xml_node root = doc.child("NotepadPlus");
    if (!root) return {};
    pugi::xml_node nl = root.child("Native-Langue");
    if (!nl) return {};
    const char* name = nl.attribute("name").value();
    return (name && *name) ? QString::fromUtf8(name) : QString();
}

// Parse a language XML's <Menu><Main> tree into st().idToText,
// st().idToSubMenuText, and st().idToCommandText. Also parses
// <Dialog>/<DialogKey> blocks into st().idToDialogText +
// st().dialogKeyToTitle (Phase 8b-polish).
void loadXmlIntoState(const QString& resource)
{
    st().idToText.clear();
    st().idToSubMenuText.clear();
    st().idToCommandText.clear();
    st().idToDialogText.clear();
    st().dialogKeyToTitle.clear();

    QFile f(resource);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray bytes = f.readAll();
    f.close();

    pugi::xml_document doc;
    const pugi::xml_parse_result r = doc.load_buffer(bytes.constData(),
        static_cast<size_t>(bytes.size()));
    if (!r) return;

    pugi::xml_node root = doc.child("NotepadPlus");
    if (!root) return;
    pugi::xml_node menu = root.child("Native-Langue").child("Menu");
    if (!menu) return;

    pugi::xml_node main = menu.child("Main");
    if (!main) return;

    // Top-level menus: <Entries><Item menuId="file" name="&amp;File"/>...</Entries>
    if (pugi::xml_node entries = main.child("Entries")) {
        for (pugi::xml_node it : entries.children("Item")) {
            const char* idV   = it.attribute("menuId").value();
            const char* nameV = it.attribute("name").value();
            if (idV && *idV && nameV) {
                st().idToText.insert(QString::fromUtf8(idV),
                                     QString::fromUtf8(nameV));
            }
        }
    }

    // Sub-menus: <SubEntries><Item subMenuId="file-recentFiles" name="..."/>
    if (pugi::xml_node subs = main.child("SubEntries")) {
        for (pugi::xml_node it : subs.children("Item")) {
            const char* idV   = it.attribute("subMenuId").value();
            const char* nameV = it.attribute("name").value();
            if (idV && *idV && nameV) {
                st().idToSubMenuText.insert(QString::fromUtf8(idV),
                                            QString::fromUtf8(nameV));
            }
        }
    }

    // Leaf actions: <Commands><Item id="41001" name="&amp;New"/>...</Commands>
    // Keyed by decimal IDM number (string form so the QHash type matches).
    if (pugi::xml_node cmds = main.child("Commands")) {
        for (pugi::xml_node it : cmds.children("Item")) {
            const char* idV   = it.attribute("id").value();
            const char* nameV = it.attribute("name").value();
            if (idV && *idV && nameV) {
                st().idToCommandText.insert(QString::fromUtf8(idV),
                                            QString::fromUtf8(nameV));
            }
        }
    }

    // Phase 8b-polish — <Dialog>/<DialogKey> blocks. Each child of
    // <Dialog> is a per-dialog group, e.g. <Find>, <Hash>, <Preference>.
    // The group element's `title` attribute is the dialog's window
    // title; child <Item id="N" name="..."/> rows are widget labels.
    pugi::xml_node dialogRoot = root.child("Native-Langue").child("Dialog");
    if (dialogRoot) {
        for (pugi::xml_node grp : dialogRoot.children()) {
            const char* keyV = grp.name();
            if (!keyV || !*keyV) continue;
            const QString key = QString::fromUtf8(keyV);

            // Window title.
            const char* titleV = grp.attribute("title").value();
            if (titleV && *titleV) {
                st().dialogKeyToTitle.insert(key, QString::fromUtf8(titleV));
            }

            // Per-id translated text. First-occurrence wins per id
            // (rare upstream duplicates within one dialog block).
            QHash<QString, QString>& idMap = st().idToDialogText[key];
            for (pugi::xml_node it : grp.children("Item")) {
                const char* idV   = it.attribute("id").value();
                const char* nameV = it.attribute("name").value();
                if (!idV || !*idV || !nameV) continue;
                const QString idStr = QString::fromUtf8(idV);
                if (!idMap.contains(idStr)) {
                    idMap.insert(idStr, QString::fromUtf8(nameV));
                }
            }
        }
    }
}

// Phase 8b — build the source-text index from english.xml's <Commands>.
// Maps normalised English text → numeric IDM string. Used regardless of
// which language is active. Built once at init() time.
void buildEnglishSourceIndex()
{
    st().englishTextToCommandId.clear();

    QFile f(QString::fromLatin1(kEnglishResource));
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray bytes = f.readAll();
    f.close();

    pugi::xml_document doc;
    const pugi::xml_parse_result r = doc.load_buffer(bytes.constData(),
        static_cast<size_t>(bytes.size()));
    if (!r) return;

    pugi::xml_node root = doc.child("NotepadPlus");
    if (!root) return;
    pugi::xml_node main = root.child("Native-Langue").child("Menu").child("Main");
    if (!main) return;

    pugi::xml_node cmds = main.child("Commands");
    if (!cmds) return;

    // First-occurrence wins — upstream emits main-menu commands before
    // tooltips/toolbar duplicates. insertOrAssign would let later
    // duplicates overwrite, so we explicitly skip when already present.
    for (pugi::xml_node it : cmds.children("Item")) {
        const char* idV   = it.attribute("id").value();
        const char* nameV = it.attribute("name").value();
        if (!idV || !*idV || !nameV) continue;

        const QString key = Localization::normaliseEnglish(QString::fromUtf8(nameV));
        if (key.isEmpty()) continue;
        if (st().englishTextToCommandId.contains(key)) continue;
        st().englishTextToCommandId.insert(key, QString::fromUtf8(idV));
    }

    // Phase 8b-polish — also build the per-dialog source-text index
    // from english.xml's <Dialog>/<DialogKey>/<Item id="N" name="..."/>.
    st().englishDialogTextToId.clear();
    pugi::xml_node dialogRoot = root.child("Native-Langue").child("Dialog");
    if (dialogRoot) {
        for (pugi::xml_node grp : dialogRoot.children()) {
            const char* keyV = grp.name();
            if (!keyV || !*keyV) continue;
            const QString key = QString::fromUtf8(keyV);
            QHash<QString, QString>& engMap = st().englishDialogTextToId[key];
            for (pugi::xml_node it : grp.children("Item")) {
                const char* idV   = it.attribute("id").value();
                const char* nameV = it.attribute("name").value();
                if (!idV || !*idV || !nameV) continue;
                const QString norm = Localization::normaliseEnglish(
                    QString::fromUtf8(nameV));
                if (norm.isEmpty()) continue;
                if (!engMap.contains(norm)) {
                    engMap.insert(norm, QString::fromUtf8(idV));
                }
            }
        }
    }
}

// Phase 8b — recursive translator. Walks every action under `menu`:
//   • If the action owns a sub-menu, translate the sub-menu's title via
//     `nlSubMenuId`, then recurse.
//   • Otherwise (leaf action), translate via source-text matching using
//     the action's `nlOriginalEnglish` snapshot (set on first apply).
// If the active language has no translation for an entry, the original
// English shines through (no destructive write).
void applyToMenuRecursive(QMenu* menu)
{
    if (!menu) return;

    for (QAction* a : menu->actions()) {
        if (a->isSeparator()) continue;

        if (QMenu* sub = a->menu()) {
            // Sub-menu: title-by-property + recurse.
            const QString id = sub->property("nlSubMenuId").toString();
            if (!id.isEmpty()) {
                const QString translated = Localization::translateSubMenu(id);
                if (!translated.isEmpty()) sub->setTitle(translated);
                else if (!Localization::active().isEmpty()) {
                    // Active language has no entry — fall back to the
                    // English source we (may have) snapshotted.
                    const QVariant orig = sub->menuAction()->property("nlOriginalEnglish");
                    if (orig.isValid()) sub->setTitle(orig.toString());
                }
            }
            applyToMenuRecursive(sub);
            continue;
        }

        // Leaf action — source-text path.
        // First apply: snapshot the English source. We test for the
        // property's *existence* (isValid) not its value, so an
        // intentionally-empty action ("(empty)") still gets snapshotted.
        const QVariant snap = a->property("nlOriginalEnglish");
        QString english;
        if (snap.isValid()) {
            english = snap.toString();
        } else {
            english = a->text();
            a->setProperty("nlOriginalEnglish", english);
        }

        if (Localization::active().isEmpty()) {
            // No language active — restore the English source. Cheap
            // no-op when text already matches.
            if (a->text() != english) a->setText(english);
            continue;
        }

        const QString translated = Localization::translateCommandByEnglish(english);
        if (!translated.isEmpty()) a->setText(translated);
        else                       a->setText(english);   // safe fallback
    }
}

} // namespace

namespace Localization {

QString normaliseEnglish(const QString& s)
{
    // Strip Qt mnemonic '&' chars (but preserve a literal && — Qt's
    // escape for '&'). Also strip trailing '...' / '…' so "Save As..."
    // matches "Save As" / "Save As…".
    QString out;
    out.reserve(s.size());
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        if (c == QLatin1Char('&')) {
            // Two consecutive '&' = literal '&'; emit one.
            if (i + 1 < s.size() && s[i + 1] == QLatin1Char('&')) {
                out.append(QLatin1Char('&'));
                ++i;
            }
            // else single '&' = mnemonic; drop.
            continue;
        }
        out.append(c);
    }
    // Trim trailing dots / ellipsis. Both ASCII "..." and Unicode "…".
    while (!out.isEmpty()) {
        const QChar tail = out[out.size() - 1];
        if (tail == QLatin1Char('.') || tail == QChar(0x2026)
            || tail == QLatin1Char(' ') || tail == QLatin1Char('\t')) {
            out.chop(1);
        } else break;
    }
    return out.trimmed();
}

void init()
{
    if (st().loaded) {
        const QString a = Config::uiLanguage();
        if (!a.isEmpty() && a != st().activeDisplay) {
            setActive(a);
        }
        return;
    }
    st().loaded = true;

    st().registry.clear();
    for (const BundledEntry& e : kBundled) {
        const QString resource = QString::fromLatin1(e.resource);
        const QString display  = readDisplayName(resource);
        if (display.isEmpty()) continue;        // resource missing or malformed
        st().registry.push_back({display, resource});
    }
    std::sort(st().registry.begin(), st().registry.end(),
        [](const Lang& a, const Lang& b) {
            return a.displayName.localeAwareCompare(b.displayName) < 0;
        });

    // Phase 8b — build the English source-text index once. Used by every
    // applyToMenuBar call regardless of the active language.
    buildEnglishSourceIndex();

    const QString persisted = Config::uiLanguage();
    if (!persisted.isEmpty()) setActive(persisted);
}

QVector<Lang> available()
{
    return st().registry;
}

QString active()
{
    return st().activeDisplay;
}

void setActive(const QString& displayName)
{
    if (displayName.isEmpty()) {
        st().activeDisplay.clear();
        st().idToText.clear();
        st().idToSubMenuText.clear();
        st().idToCommandText.clear();
        st().idToDialogText.clear();
        st().dialogKeyToTitle.clear();
        Config::setUiLanguage(QString());
        return;
    }

    // Find the matching registry entry.
    for (const Lang& l : st().registry) {
        if (l.displayName == displayName) {
            loadXmlIntoState(l.resourcePath);
            st().activeDisplay = displayName;
            Config::setUiLanguage(displayName);
            return;
        }
    }
    // Unknown name — silently ignore (config might point at a language
    // we no longer ship).
}

QString translate(const QString& menuId)
{
    return st().idToText.value(menuId);
}

QString translateSubMenu(const QString& subMenuId)
{
    return st().idToSubMenuText.value(subMenuId);
}

QString translateCommandByEnglish(const QString& english)
{
    if (english.isEmpty()) return {};
    const QString key = normaliseEnglish(english);
    if (key.isEmpty()) return {};
    const QString id = st().englishTextToCommandId.value(key);
    if (id.isEmpty()) return {};
    return st().idToCommandText.value(id);
}

void applyToMenuBar(QMenuBar* mb)
{
    if (!mb) return;
    for (QAction* a : mb->actions()) {
        QMenu* m = a->menu();
        if (!m) continue;

        // Top-level title via nlMenuId. First apply snapshots the
        // current (English) title into nlOriginalEnglish so subsequent
        // language switches translate from the snapshot rather than
        // from already-translated text.
        const QString id = m->property("nlMenuId").toString();
        if (!id.isEmpty()) {
            QAction* menuAct = m->menuAction();
            QString english;
            const QVariant snap = menuAct->property("nlOriginalEnglish");
            if (snap.isValid()) {
                english = snap.toString();
            } else {
                english = m->title();
                menuAct->setProperty("nlOriginalEnglish", english);
            }

            if (active().isEmpty()) {
                if (m->title() != english) m->setTitle(english);
            } else {
                const QString translated = translate(id);
                m->setTitle(translated.isEmpty() ? english : translated);
            }
        }

        // Phase 8b — recurse into sub-menus + leaf actions.
        applyToMenuRecursive(m);
    }
}

QString translateDialogTitle(const char* dialogKey)
{
    if (!dialogKey || !*dialogKey) return {};
    return st().dialogKeyToTitle.value(QString::fromUtf8(dialogKey));
}

namespace {

// Phase 8b-polish — translate a single widget's text via the dialog
// source-text index. `current` is the widget's currently-displayed
// text (after the first apply this comes from the nlOriginalEnglish
// snapshot, NOT from the live widget — otherwise re-applies would
// translate already-translated text and miss).
QString translateDialogWidgetText(const QString& dialogKey,
                                  const QString& currentEnglish)
{
    if (currentEnglish.isEmpty()) return {};
    const QString norm = Localization::normaliseEnglish(currentEnglish);
    if (norm.isEmpty()) return {};
    const auto engMap = st().englishDialogTextToId.value(dialogKey);
    const QString id = engMap.value(norm);
    if (id.isEmpty()) return {};
    const auto idMap = st().idToDialogText.value(dialogKey);
    return idMap.value(id);
}

// Phase 8b-polish — apply translation to one widget. Snapshots the
// original English to nlOriginalEnglish on first call; reads the
// snapshot on subsequent calls. setText() and text() pair via a
// templated lambda so this works for QLabel/QCheckBox/QPushButton/
// QRadioButton (all have text()/setText()) and QGroupBox (uses
// title()/setTitle() — handled with a separate overload).
template <typename W>
void translateOne(W* w, const QString& dialogKey)
{
    if (!w) return;
    QString english;
    const QVariant snap = w->property("nlOriginalEnglish");
    if (snap.isValid()) {
        english = snap.toString();
    } else {
        english = w->text();
        w->setProperty("nlOriginalEnglish", english);
    }
    if (Localization::active().isEmpty()) {
        if (w->text() != english) w->setText(english);
        return;
    }
    const QString translated = translateDialogWidgetText(dialogKey, english);
    w->setText(translated.isEmpty() ? english : translated);
}

// QGroupBox uses title()/setTitle() instead of text()/setText().
void translateOneGroupBox(QGroupBox* w, const QString& dialogKey)
{
    if (!w) return;
    QString english;
    const QVariant snap = w->property("nlOriginalEnglish");
    if (snap.isValid()) {
        english = snap.toString();
    } else {
        english = w->title();
        w->setProperty("nlOriginalEnglish", english);
    }
    if (Localization::active().isEmpty()) {
        if (w->title() != english) w->setTitle(english);
        return;
    }
    const QString translated = translateDialogWidgetText(dialogKey, english);
    w->setTitle(translated.isEmpty() ? english : translated);
}

} // namespace

void applyToDialog(QWidget* widget, const char* dialogKey)
{
    if (!widget || !dialogKey || !*dialogKey) return;
    const QString key = QString::fromUtf8(dialogKey);

    // Window title — works for both QDialog and QDockWidget; both
    // expose windowTitle() with semantically equivalent behaviour.
    {
        QString englishTitle;
        const QVariant snap = widget->property("nlOriginalEnglish");
        if (snap.isValid()) {
            englishTitle = snap.toString();
        } else {
            englishTitle = widget->windowTitle();
            widget->setProperty("nlOriginalEnglish", englishTitle);
        }
        if (active().isEmpty()) {
            if (widget->windowTitle() != englishTitle)
                widget->setWindowTitle(englishTitle);
        } else {
            const QString t = translateDialogTitle(dialogKey);
            widget->setWindowTitle(t.isEmpty() ? englishTitle : t);
        }
    }

    // Walk every translatable widget kind. findChildren is recursive
    // by default — picks up widgets inside QGroupBoxes / QTabWidgets /
    // any nested layout.
    for (QLabel*       w : widget->findChildren<QLabel*>())       translateOne(w, key);
    for (QCheckBox*    w : widget->findChildren<QCheckBox*>())    translateOne(w, key);
    for (QPushButton*  w : widget->findChildren<QPushButton*>())  translateOne(w, key);
    for (QRadioButton* w : widget->findChildren<QRadioButton*>()) translateOne(w, key);
    for (QGroupBox*    w : widget->findChildren<QGroupBox*>())    translateOneGroupBox(w, key);
}

} // namespace Localization
