<p align="center">
  <img src="logo.png" alt="padnote-- logo" width="160" height="160">
</p>

<h1 align="center">padnote-- 1.0</h1>

A Qt 6 text editor for Linux. Multi-tab editing, syntax highlighting
for ~85 languages, light/dark theme, find/replace with regex, session
restore, encoding detection, multi-cursor, find-in-files, syntax-
highlighted print, dock panels (Document Map / Function List / File
Browser / Project Panel), user-defined languages, plugin runtime
(dlopen-based), and proper desktop integration.

## Build from source

Requires CMake ≥ 3.20, Qt 6.4+, GCC 11+ or Clang 14+, C++20, and the
system development headers for CUPS and D-Bus.

### Ubuntu / Debian / Pop!_OS

```bash
sudo apt install \
    build-essential cmake ninja-build \
    qt6-base-dev qt6-base-private-dev qt6-tools-dev qt6-5compat-dev \
    libcups2-dev libdbus-1-dev libxkbcommon-dev
cmake -B build -G Ninja
cmake --build build -j$(nproc)
./build/padnote
```

### Fedora / RHEL clones

```bash
sudo dnf install \
    cmake ninja-build gcc-c++ \
    qt6-qtbase-devel qt6-qttools-devel qt6-qt5compat-devel \
    cups-devel dbus-devel libxkbcommon-devel
cmake -B build -G Ninja && cmake --build build -j$(nproc)
```

### Arch / Manjaro

```bash
sudo pacman -S --needed \
    base-devel cmake ninja \
    qt6-base qt6-tools qt6-5compat \
    cups dbus libxkbcommon
cmake -B build -G Ninja && cmake --build build -j$(nproc)
```

### System install

```bash
sudo cmake --install build               # /usr/local by default
sudo update-desktop-database  /usr/local/share/applications
sudo update-mime-database     /usr/local/share/mime
sudo gtk-update-icon-cache    -f -t /usr/local/share/icons/hicolor
```

After this, padnote-- shows up in the GNOME / KDE / XFCE application
launcher under "Programming" / "Utilities", and right-click → Open With
on common code files lists it.

## Where things live

```
~/.config/padnote/padnote--/
    config.xml            # theme + window geometry + recent files
    session.xml           # open tabs + per-tab cursor positions
    userDefineLangs.xml   # user-defined language definitions
    plugins/              # user plugins (.so files)
    themes/               # user themes (.xml stylers)
    backup/               # crash-recovery snapshots
```

## Quick start

| Action | Shortcut |
| --- | --- |
| New file | `Ctrl+N` |
| Open... | `Ctrl+O` |
| Save | `Ctrl+S` |
| Save As... | `Ctrl+Shift+S` |
| Reload from disk | `Ctrl+R` |
| Find... | `Ctrl+F` |
| Replace... | `Ctrl+H` |
| Find in Files... | `Ctrl+Shift+F` |
| Go to line... | `Ctrl+G` |
| Toggle bookmark | `Ctrl+F2` |
| Block Comment / Uncomment | `Ctrl+Q` |
| Stream Comment | `Ctrl+Shift+Q` |
| Add cursor at next match | `Ctrl+D` |
| Document Switcher | `Ctrl+Tab` |
| Run external command... | `F5` |
| Print... | `Ctrl+P` |

## Single-instance behaviour

`padnote path/to/file.txt` from a terminal — or clicking a file in your
file manager — opens the file in your existing window (and raises it to
the foreground), via a D-Bus service registered on the user's session
bus (`org.padnote.Instance`).

To always open a fresh window:

```bash
padnote --new-instance some-file.txt
```

## Licensing

GPL-3.0-or-later. See [LICENSE](LICENSE) for the full GPL text and
[LICENSE_PORT.md](LICENSE_PORT.md) for the per-component breakdown
(Scintilla HPND, Lexilla HPND, Boost.Regex BSL-1.0, PugiXML MIT,
uchardet MPL-1.1) and project lineage.
