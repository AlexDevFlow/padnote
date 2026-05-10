// linuxplugin.h — Public ABI for padnote-- plugins.
//
// This header is the contract between the host (padnote binary) and a
// plugin (a .so file living at
// ~/.config/padnote/padnote--/plugins/<name>/<name>.so).
//
// Types that come from the legacy Win32 plugin world are mapped to
// portable Linux equivalents:
//   • HWND → void* (opaque handle, never dereferenced by plugins)
//   • WPARAM / LPARAM → uintptr_t / intptr_t
//   • LRESULT → intptr_t
//   • BOOL → int (0 / non-zero)
//   • SCNotification → forward-declared struct; plugins should #include
//     <Scintilla.h> for the full definition (Scintilla itself is
//     platform-portable).
//
// Plugins should be built with `-fvisibility=hidden -fvisibility-inlines-hidden`
// and explicitly export the required symbols via
// `extern "C" __attribute__((visibility("default")))`.
//
// The host loads plugins via dlopen + dlsym; missing required symbols
// causes the load to fail with a status-bar message, not a crash.

#ifndef PADNOTE_LINUX_PLUGIN_H
#define PADNOTE_LINUX_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Opaque host handles --------------------------------------------------

// The host's MainWindow. Pass back to messageProc / Scintilla calls.
// Plugins MUST NOT cast to a concrete type — the underlying object's
// layout is private to the host.
typedef void* NppHandle;

// A ScintillaEditBase instance. Same opacity rules.
typedef void* ScintillaHandle;

// ---- Initialization data --------------------------------------------------

// Passed to setInfo() once after dlopen, before any other entry-point
// is called. Plugins typically cache this in a static variable.
typedef struct LinuxNppData {
    NppHandle       nppHandle;
    ScintillaHandle scintillaMainHandle;     // active pane's editor
    ScintillaHandle scintillaSecondHandle;   // other pane (or NULL)
} LinuxNppData;

// ---- Shortcut descriptors -------------------------------------------------

// Optional shortcut for a FuncItem. NULL = no shortcut. The `key` value
// is a Qt::Key (e.g. Qt::Key_F5 = 0x01000034).
typedef struct LinuxShortcutKey {
    int isCtrl;
    int isAlt;
    int isShift;
    int key;
} LinuxShortcutKey;

// ---- Menu items -----------------------------------------------------------

// Maximum length of a Plugins-menu item name (UTF-8 bytes, including
// the trailing NUL). Matches upstream's `menuItemSize = 64`.
#define LINUX_PLUGIN_MENU_ITEM_SIZE 64

// One menu entry the plugin contributes. The host renders these as
// children of Plugins → <plugin name> → <itemName>. Clicking calls
// `func()` with no arguments — plugins read editor state via the
// cached NppData.
typedef struct LinuxFuncItem {
    char                itemName[LINUX_PLUGIN_MENU_ITEM_SIZE];
    void              (*func)(void);
    int                 cmdID;          // host fills this in
    int                 init2Check;     // initial check state (for toggleable items)
    LinuxShortcutKey*   pShKey;         // nullable
} LinuxFuncItem;

// ---- Required exported entry points ---------------------------------------
//
// Every plugin MUST export these five symbols. Missing or null returns
// from any of them aborts the load.

// Plugin's display name. UTF-8, NUL-terminated. Plugin retains
// ownership of the buffer (typically a static string literal).
const char* getName(void);

// Called once after dlopen. Plugin caches `data` for use by command
// callbacks (which receive no arguments).
void setInfo(LinuxNppData data);

// Returns the plugin's command list. `*count` is set to the number
// of FuncItems. Plugin retains ownership of the array (typically a
// static array). Host renders the items into the Plugins submenu in
// the order returned.
LinuxFuncItem* getFuncsArray(int* count);

// Editor notification callback. The `notification` pointer is an
// SCNotification* (Scintilla notification — same struct as upstream's
// Win32 build, since Scintilla is platform-portable). Plugins should
// `#include <Scintilla.h>` for the full struct.
//
// Called for SCN_* events on the active editor (focus change, text
// modified, save point reached, etc.). Plugins receiving notifications
// from a different pane than the cached scintillaMainHandle should
// consult `notification->nmhdr.idFrom` (or the equivalent field for
// the host's mapping).
void beNotified(void* notification);

// Message dispatch — plugins use this to send the host arbitrary
// commands (e.g. force-reload-buffer, toggle-fold-at-line, etc.).
// `msg` is one of the NPPM_* constants from Notepad_plus_msgs.h
// (which the host re-exports as a Linux-friendly subset; see the
// linuxplugin-msgs.h companion when that lands in v4.1).
//
// Return value semantics depend on the message — most return 0 on
// success, non-zero on error.
intptr_t messageProc(unsigned int msg, uintptr_t wParam, intptr_t lParam);

#ifdef __cplusplus
}
#endif

#endif // PADNOTE_LINUX_PLUGIN_H
