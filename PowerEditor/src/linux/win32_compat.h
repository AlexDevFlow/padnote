// win32_compat.h — minimal Win32-types shim.
//
// Some legacy code in the tree uses Win32 typedefs in files that don't
// otherwise touch the Win32 API (UCHAR, BYTE, DWORD, COLORREF, ...).
// This header provides Linux-side aliases so those types resolve. It
// is NOT a complete Win32 replacement — anything that actually *calls*
// a Win32 API (CreatePopupMenu, SendMessage on a non-Scintilla HWND,
// RegOpenKey, ...) will still fail to link.

#pragma once

#ifndef _WIN32

#include <cstdint>

// --- Scalar typedefs -----------------------------------------------------

using BYTE   = unsigned char;
using UCHAR  = unsigned char;
using WORD   = unsigned short;
using USHORT = unsigned short;
using DWORD  = unsigned int;            // 32 bits on Win32 AND on Linux x86_64
using ULONG  = unsigned int;
using LONG   = int;
using INT    = int;
using UINT   = unsigned int;
using BOOL   = int;
using LONGLONG  = long long;
using ULONGLONG = unsigned long long;
using LARGE_INTEGER  = long long;
using ULARGE_INTEGER = unsigned long long;

using COLORREF = unsigned int;          // 0x00BBGGRR layout (preserved)

// Win32 boolean values
constexpr BOOL TRUE  = 1;
constexpr BOOL FALSE = 0;

// --- Opaque handle types -------------------------------------------------
//
// We give each handle type its own incomplete struct so the compiler keeps
// them distinct (a HMENU cannot be silently passed where a HWND is expected).
// The actual "value" of these handles is never dereferenced on Linux — they
// only travel through code that hasn't been rewritten yet, and the linker
// will catch any code that tries to act on them via a Win32 API.

#define NPP_DECLARE_OPAQUE_HANDLE(name) struct name##__ ; using name = struct name##__ *

NPP_DECLARE_OPAQUE_HANDLE(HWND);
NPP_DECLARE_OPAQUE_HANDLE(HMENU);
NPP_DECLARE_OPAQUE_HANDLE(HINSTANCE);
NPP_DECLARE_OPAQUE_HANDLE(HMODULE);
NPP_DECLARE_OPAQUE_HANDLE(HDC);
NPP_DECLARE_OPAQUE_HANDLE(HBITMAP);
NPP_DECLARE_OPAQUE_HANDLE(HFONT);
NPP_DECLARE_OPAQUE_HANDLE(HBRUSH);
NPP_DECLARE_OPAQUE_HANDLE(HPEN);
NPP_DECLARE_OPAQUE_HANDLE(HRGN);
NPP_DECLARE_OPAQUE_HANDLE(HICON);
NPP_DECLARE_OPAQUE_HANDLE(HCURSOR);
NPP_DECLARE_OPAQUE_HANDLE(HACCEL);
NPP_DECLARE_OPAQUE_HANDLE(HKEY);
NPP_DECLARE_OPAQUE_HANDLE(HHOOK);
NPP_DECLARE_OPAQUE_HANDLE(HIMAGELIST);

#undef NPP_DECLARE_OPAQUE_HANDLE

// HRESULT / LRESULT / WPARAM / LPARAM — match Windows widths so tons of
// existing helper functions keep their signatures.
using HRESULT = long;
using LRESULT = std::intptr_t;
using WPARAM  = std::uintptr_t;
using LPARAM  = std::intptr_t;

// --- Geometry ------------------------------------------------------------

struct RECT { LONG left, top, right, bottom; };
struct POINT { LONG x, y; };
struct SIZE  { LONG cx, cy; };

// --- Macros that show up in casual code ----------------------------------

#ifndef MAX_PATH
#  define MAX_PATH 260   // matches Windows; Linux PATH_MAX is 4096 but we
                         // mostly use this for fixed-size buffers in upstream
                         // dialogs. Will revisit when we actually port them.
#endif

#ifndef CALLBACK
#  define CALLBACK
#endif
#ifndef WINAPI
#  define WINAPI
#endif

// String type the upstream code uses interchangeably with std::wstring.
// We keep wstring on Linux for Phases 1–3; see PORTING_NOTES.md §4.4 for the
// long-term migration plan.
#include <string>
using generic_string = std::wstring;

#endif // !_WIN32
