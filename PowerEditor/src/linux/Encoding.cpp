#include "Encoding.h"

#include <QTextCodec>

#include <cstring>

#include "uchardet.h"

namespace {

constexpr unsigned char kBomUtf8[]    = {0xEF, 0xBB, 0xBF};
constexpr unsigned char kBomUtf16Be[] = {0xFE, 0xFF};
constexpr unsigned char kBomUtf16Le[] = {0xFF, 0xFE};
constexpr unsigned char kBomUtf32Be[] = {0x00, 0x00, 0xFE, 0xFF};
constexpr unsigned char kBomUtf32Le[] = {0xFF, 0xFE, 0x00, 0x00};

bool startsWith(const QByteArray& b, const unsigned char* m, std::size_t n)
{
    if (static_cast<std::size_t>(b.size()) < n) return false;
    return std::memcmp(b.constData(), m, n) == 0;
}

bool stripBom(QByteArray& b, EncodingInfo& out)
{
    // UTF-32 BOM detection must precede UTF-16 because UTF-32-LE starts with
    // FF FE 00 00 (which would otherwise look like UTF-16-LE).
    if (startsWith(b, kBomUtf32Le, 4)) {
        b.remove(0, 4); out.name = QStringLiteral("UTF-32LE"); out.hasBom = true; return true;
    }
    if (startsWith(b, kBomUtf32Be, 4)) {
        b.remove(0, 4); out.name = QStringLiteral("UTF-32BE"); out.hasBom = true; return true;
    }
    if (startsWith(b, kBomUtf8, 3)) {
        b.remove(0, 3); out.name = QStringLiteral("UTF-8");    out.hasBom = true; return true;
    }
    if (startsWith(b, kBomUtf16Le, 2)) {
        b.remove(0, 2); out.name = QStringLiteral("UTF-16LE"); out.hasBom = true; return true;
    }
    if (startsWith(b, kBomUtf16Be, 2)) {
        b.remove(0, 2); out.name = QStringLiteral("UTF-16BE"); out.hasBom = true; return true;
    }
    return false;
}

QTextCodec* codecForOrFallback(const QString& name, QTextCodec* fallback)
{
    QTextCodec* c = QTextCodec::codecForName(name.toLatin1());
    return c ? c : fallback;
}

// Strict UTF-8 validity check. Rejects:
//   - high bytes that aren't valid UTF-8 lead bytes
//   - truncated multi-byte sequences
//   - missing continuation bytes (top bits not 10xxxxxx)
//
// Used as a tiebreaker when uchardet returns no opinion (typical for very
// short or ambiguous samples).
bool isValidUtf8(const QByteArray& bytes)
{
    const unsigned char* p   = reinterpret_cast<const unsigned char*>(bytes.constData());
    const unsigned char* end = p + bytes.size();
    while (p < end) {
        const unsigned char c = *p++;
        if (c < 0x80) continue;                 // ASCII
        int extra;
        if      ((c & 0xE0) == 0xC0) extra = 1; // 110xxxxx
        else if ((c & 0xF0) == 0xE0) extra = 2; // 1110xxxx
        else if ((c & 0xF8) == 0xF0) extra = 3; // 11110xxx
        else return false;                      // invalid lead byte
        if (p + extra > end) return false;      // truncated
        for (int i = 0; i < extra; ++i) {
            if ((*p++ & 0xC0) != 0x80) return false;   // bad continuation
        }
    }
    return true;
}

} // namespace

namespace Encoding {

EncodingInfo defaultEncoding()
{
    return {QStringLiteral("UTF-8"), false};
}

EncodingInfo detect(QByteArray& bytes)
{
    EncodingInfo out;
    if (stripBom(bytes, out)) return out;

    if (bytes.isEmpty()) return defaultEncoding();

    uchardet_t ud = uchardet_new();
    uchardet_handle_data(ud, bytes.constData(),
                         static_cast<std::size_t>(bytes.size()));
    uchardet_data_end(ud);
    const char* charset = uchardet_get_charset(ud);
    QString detected;
    if (charset && *charset) detected = QString::fromLatin1(charset);
    uchardet_delete(ud);

    // If uchardet had no opinion (typical for very short samples) or said
    // ASCII (pure 7-bit), use UTF-8/Windows-1252 based on actual byte
    // content: if the bytes are valid UTF-8, UTF-8; otherwise Windows-1252.
    // Without this fallback, a tiny Latin-1 file like "café\n" (5 bytes)
    // gets mis-decoded as UTF-8, producing U+FFFD glyphs.
    if (detected.isEmpty() || detected.compare(QStringLiteral("ASCII"),
                                               Qt::CaseInsensitive) == 0) {
        if (isValidUtf8(bytes)) return defaultEncoding();
        return EncodingInfo{QStringLiteral("windows-1252"), false};
    }

    // Sanity-check: does Qt know the codec? If not, do the same fallback.
    if (!QTextCodec::codecForName(detected.toLatin1())) {
        if (isValidUtf8(bytes)) return defaultEncoding();
        return EncodingInfo{QStringLiteral("windows-1252"), false};
    }

    out.name   = detected;
    out.hasBom = false;
    return out;
}

QString decode(const QByteArray& bytes, const EncodingInfo& enc)
{
    QTextCodec* utf8 = QTextCodec::codecForName("UTF-8");
    QTextCodec* latin1 = QTextCodec::codecForName("ISO-8859-1");
    QTextCodec* fallback = utf8 ? utf8 : latin1;
    QTextCodec* codec = codecForOrFallback(enc.name, fallback);
    return codec ? codec->toUnicode(bytes) : QString::fromLatin1(bytes);
}

QByteArray encode(const QString& text, const EncodingInfo& enc)
{
    QTextCodec* utf8 = QTextCodec::codecForName("UTF-8");
    QTextCodec* codec = codecForOrFallback(enc.name, utf8);

    QByteArray out;
    if (enc.hasBom) {
        if      (enc.name.compare(QStringLiteral("UTF-8"),    Qt::CaseInsensitive) == 0) {
            out.append(reinterpret_cast<const char*>(kBomUtf8),    3);
        } else if (enc.name.compare(QStringLiteral("UTF-16BE"), Qt::CaseInsensitive) == 0) {
            out.append(reinterpret_cast<const char*>(kBomUtf16Be), 2);
        } else if (enc.name.compare(QStringLiteral("UTF-16LE"), Qt::CaseInsensitive) == 0) {
            out.append(reinterpret_cast<const char*>(kBomUtf16Le), 2);
        } else if (enc.name.compare(QStringLiteral("UTF-32BE"), Qt::CaseInsensitive) == 0) {
            out.append(reinterpret_cast<const char*>(kBomUtf32Be), 4);
        } else if (enc.name.compare(QStringLiteral("UTF-32LE"), Qt::CaseInsensitive) == 0) {
            out.append(reinterpret_cast<const char*>(kBomUtf32Le), 4);
        }
    }
    if (codec) {
        // QTextCodec::ConverterState lets us avoid emitting Qt's own BOM
        // (we already prefixed it ourselves above) on UTF-16/32.
        QTextCodec::ConverterState state(QTextCodec::IgnoreHeader);
        out.append(codec->fromUnicode(text.constData(), text.size(), &state));
    } else {
        out.append(text.toUtf8());
    }
    return out;
}

QString displayName(const EncodingInfo& enc)
{
    if (enc.name.compare(QStringLiteral("UTF-8"), Qt::CaseInsensitive) == 0) {
        return enc.hasBom ? QStringLiteral("UTF-8 BOM") : QStringLiteral("UTF-8");
    }
    if (enc.name.compare(QStringLiteral("UTF-16BE"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("UTF-16 BE BOM");
    }
    if (enc.name.compare(QStringLiteral("UTF-16LE"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("UTF-16 LE BOM");
    }
    if (enc.name.compare(QStringLiteral("windows-1252"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("ANSI (Windows-1252)");
    }
    return enc.name;
}

} // namespace Encoding
