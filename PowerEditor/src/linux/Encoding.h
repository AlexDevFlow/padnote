// Encoding.h — Phase 5c: file encoding detection + transcoding helpers.
//
// Wraps:
//   - uchardet (PowerEditor/src/uchardet/) for byte-frequency detection
//     when no BOM is present.
//   - QTextCodec (Qt6Core5Compat) for the broad set of legacy charsets
//     (Windows-1252, ISO-8859-*, Shift_JIS, GB18030, Big5, EUC-*, KOI8-*,
//     etc.) — Qt6's native QStringConverter only covers UTF-* + Latin-1.
//
// The internal Buffer always holds UTF-8 (Scintilla SC_CP_UTF8). Encoding
// is purely a file-I/O concern: load decodes bytes → UTF-8; save re-encodes
// UTF-8 → bytes.

#pragma once

#include <QByteArray>
#include <QString>

struct EncodingInfo {
    // Codec name acceptable to QTextCodec::codecForName (e.g. "UTF-8",
    // "windows-1252", "Shift_JIS", "UTF-16LE"). Always non-empty after init.
    QString name;

    // True if a BOM should be written on save and was stripped on load.
    bool hasBom = false;

    // Equality for menu-radio bookkeeping.
    bool operator==(const EncodingInfo& o) const {
        return name == o.name && hasBom == o.hasBom;
    }
    bool operator!=(const EncodingInfo& o) const { return !(*this == o); }
};

namespace Encoding {

// The default for new untitled buffers.
EncodingInfo defaultEncoding();

// Inspect raw file bytes to decide what encoding they're in.
// 1. If a BOM is present (UTF-8, UTF-16BE, UTF-16LE, UTF-32BE, UTF-32LE),
//    use it and report hasBom=true. Strip the BOM bytes from 'bytes' as a
//    side effect (caller passes a non-const ref).
// 2. Else run uchardet. If it returns a non-empty charset that QTextCodec
//    recognises, use it.
// 3. Else fall back to UTF-8.
//
// 'bytes' is mutated only when a BOM is stripped.
EncodingInfo detect(QByteArray& bytes);

// Decode 'bytes' (already BOM-stripped if applicable) using the codec named
// by 'enc'. Falls back to Latin-1 if the codec is unknown — never throws.
QString decode(const QByteArray& bytes, const EncodingInfo& enc);

// Encode 'text' to bytes using the codec named by 'enc'. Returns BOM bytes
// + encoded text when enc.hasBom is true. Falls back to UTF-8 if the codec
// is unknown.
QByteArray encode(const QString& text, const EncodingInfo& enc);

// Human-readable display name for a status bar (e.g., "UTF-8", "UTF-8 BOM",
// "ANSI (Windows-1252)").
QString displayName(const EncodingInfo& enc);

} // namespace Encoding
