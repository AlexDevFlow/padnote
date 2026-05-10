# Licensing — padnote--

This document summarises the licensing situation for padnote--. It does
not replace the source-text licences in each file — those remain
authoritative.

## Project licence

padnote-- is licensed under the **GNU General Public License, version 3
or later (GPL-3.0-or-later)**, inherited from the codebase it derives
from.

The full text of GPL-3.0 is in the [LICENSE](LICENSE) file at the
repository root.

Practical implications:

- Any binary distribution (including AppImage builds, `.deb` / RPM
  packages, Flatpak bundles, and source tarballs) **must** ship the
  corresponding source code and grant downstream users the freedoms
  enumerated in GPL-3.0+.
- Redistributors may add their own modifications — but those must also
  be GPL-3.0+ and source-available to anyone who receives the binary.
- padnote-- can link against LGPL libraries (Qt 6) without those
  libraries becoming part of the GPL'd combined work; the Qt 6 LGPL
  exception is detailed below.

## Linkage and the LGPL Qt clarification

padnote-- depends on **Qt 6.4+ Widgets / Core / Gui / Core5Compat /
PrintSupport / DBus**. Qt 6 modules are dual-licensed (commercial +
LGPL-3.0). padnote-- uses the **LGPL-3.0-only** copies, which is
compatible with GPL-3.0+ under the standard "copyleft" interpretation:

- We dynamically link against the system Qt installation. End users can
  swap the Qt libraries (drop-in or LD_LIBRARY_PATH) without touching
  this binary.
- The combined work is GPL-3.0+; Qt itself remains LGPL-3.0.
- AppImage builds bundle Qt libraries inside the AppImage payload. This
  is permitted under LGPL-3.0 §6 because the bundled `.so` files retain
  their individual LGPL licence and source is available from the Qt
  project.

If you redistribute an AppImage build, also ship a pointer to the Qt
source release matching the bundled version
(https://download.qt.io/archive/qt/).

## Project lineage

padnote-- is built on top of a GPL-3.0+ codebase originally authored
by Don Ho (1998–2025). That codebase contributed:

- the surrounding source tree layout (PowerEditor/src/),
- the bundled language model XML (`langs.model.xml`),
- the bundled theme XMLs (`stylers.model.xml`, `DarkModeDefault.xml`,
  and the per-theme files under `themes/`),
- the bundled `nativeLang/*.xml` translation files,
- the bundled `functionList/*.xml` parser definitions,
- and the User-Defined Language XML schema.

The interactive editor surface, the Linux build system, the Qt-Widgets
UI layer, and everything under `PowerEditor/src/linux/` are new code
contributed under GPL-3.0+ to the padnote-- project.

Source-file headers in upstream-derived files retain their original
copyright notices as required by GPL-3.0+ §5(a). Those notices are
load-bearing legal attribution and must not be removed under any
circumstance.

## Vendored components

The repository carries upstream copies of several third-party libraries.
Each keeps its own licence:

| Component | Path | Licence | Notes |
| --- | --- | --- | --- |
| **Scintilla** | `scintilla/` | HPND (Historical Permission Notice and Disclaimer) | Upstream from Neil Hodgson. |
| **Lexilla** | `lexilla/` | HPND | Sister project to Scintilla. |
| **Boost.Regex** | `boostregex/` (vendored at `boost_1_90_0/`) | BSL-1.0 (Boost Software License) | Used by Scintilla's regex engine. |
| **PugiXML** | `PowerEditor/src/pugixml/` | MIT | Used for `langs.model.xml` / `stylers.xml` / `config.xml` / `session.xml` parsing. |
| **uchardet** | `PowerEditor/src/uchardet/` | MPL-1.1 (Mozilla Public License) / GPL-2.0+ / LGPL-2.1+ tri-licence | Used for charset detection at file-load time. |

Some legacy hash sources (`PowerEditor/src/MISC/{md5,sha1,sha2,sha512}/`)
are present in the tree but are **not built** by padnote-- — Qt's
`QCryptographicHash` is used instead.

## Distributing modified versions

You're free to fork, modify, and redistribute padnote-- under the
GPL-3.0+ terms.

- If you fork to ship a derived editor under a different name, update
  the D-Bus service name (`SingleInstance.cpp`'s `kService`) so your
  fork doesn't collide with installs of the canonical binary on the
  same session bus.
- Update `padnote.desktop`'s `Name=` / `Exec=` / `Icon=` /
  `StartupWMClass=` fields if you rename the executable.
