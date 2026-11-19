#include "PromExporter.hpp"

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringDecoder>

#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <system_error>

// Reference for every function here: hwinfo_prom_exporter.py @ 2fb02f5
// (vendored verbatim at tools/reference/hwinfo_prom_exporter_2fb02f5.py).
// Comments name the Python function being mirrored.

namespace prom {

namespace {

// Iterate a QString as Unicode code points (Python str semantics). Unpaired
// surrogates are yielded as-is, which is also what Python would hold.
template <typename F>
void forEachCodePoint(const QString& s, F&& f)
{
    const qsizetype n = s.size();
    for (qsizetype i = 0; i < n; ++i) {
        const QChar c = s.at(i);
        if (c.isHighSurrogate() && i + 1 < n && s.at(i + 1).isLowSurrogate()) {
            f(QChar::surrogateToUcs4(c, s.at(i + 1)));
            ++i;
        } else {
            f(char32_t(c.unicode()));
        }
    }
}

// Python's strict UTF-8 decoder rules (Unicode Table 3-7): no overlongs, no
// encoded surrogates, nothing above U+10FFFF, no truncated sequences.
bool isStrictUtf8(const QByteArray& bytes)
{
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.constData());
    const qsizetype n = bytes.size();
    auto cont = [&](qsizetype i, unsigned char lo = 0x80, unsigned char hi = 0xBF) {
        return i < n && p[i] >= lo && p[i] <= hi;
    };
    qsizetype i = 0;
    while (i < n) {
        const unsigned char b0 = p[i];
        if (b0 <= 0x7F) {
            i += 1;
        } else if (b0 >= 0xC2 && b0 <= 0xDF) {
            if (!cont(i + 1)) return false;
            i += 2;
        } else if (b0 == 0xE0) {
            if (!cont(i + 1, 0xA0, 0xBF) || !cont(i + 2)) return false;
            i += 3;
        } else if ((b0 >= 0xE1 && b0 <= 0xEC) || b0 == 0xEE || b0 == 0xEF) {
            if (!cont(i + 1) || !cont(i + 2)) return false;
            i += 3;
        } else if (b0 == 0xED) {
            if (!cont(i + 1, 0x80, 0x9F) || !cont(i + 2)) return false;
            i += 3;
        } else if (b0 == 0xF0) {
            if (!cont(i + 1, 0x90, 0xBF) || !cont(i + 2) || !cont(i + 3)) return false;
            i += 4;
        } else if (b0 >= 0xF1 && b0 <= 0xF3) {
            if (!cont(i + 1) || !cont(i + 2) || !cont(i + 3)) return false;
            i += 4;
        } else if (b0 == 0xF4) {
            if (!cont(i + 1, 0x80, 0x8F) || !cont(i + 2) || !cont(i + 3)) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

const QHash<QString, QString>& unitNormalization()
{
    // UNIT_NORMALIZATION (exact keys).
    static const QHash<QString, QString> map = {
        {QString::fromUtf16(u"°C"), QStringLiteral("celsius")},
        {QStringLiteral("C"), QStringLiteral("celsius")},
        {QStringLiteral("%"), QStringLiteral("percent")},
        {QStringLiteral("V"), QStringLiteral("volts")},
        {QStringLiteral("mV"), QStringLiteral("millivolts")},
        {QStringLiteral("A"), QStringLiteral("amps")},
        {QStringLiteral("W"), QStringLiteral("watts")},
        {QStringLiteral("RPM"), QStringLiteral("rpm")},
        {QStringLiteral("MHz"), QStringLiteral("mhz")},
        {QStringLiteral("GHz"), QStringLiteral("ghz")},
        {QStringLiteral("FPS"), QStringLiteral("fps")},
        {QStringLiteral("fps"), QStringLiteral("fps")},
        {QStringLiteral("MB"), QStringLiteral("megabytes")},
        {QStringLiteral("GB"), QStringLiteral("gigabytes")},
        {QStringLiteral("KB"), QStringLiteral("kilobytes")},
        {QStringLiteral("MB/s"), QStringLiteral("megabytes_per_second")},
        {QStringLiteral("GB/s"), QStringLiteral("gigabytes_per_second")},
        {QStringLiteral("KB/s"), QStringLiteral("kilobytes_per_second")},
        {QStringLiteral("ms"), QStringLiteral("milliseconds")},
        {QStringLiteral("GT/s"), QStringLiteral("gigatransfers_per_second")},
        {QStringLiteral("x"), QStringLiteral("ratio")},
        {QStringLiteral("T"), QStringLiteral("ticks")},
        {QStringLiteral("Yes/No"), QStringLiteral("bool")},
        {QString(), QStringLiteral("none")},
    };
    return map;
}

// Python float(str) for an already-stripped string. Returns nullopt where
// Python raises ValueError. May return +-inf / nan; the caller drops those.
std::optional<double> pyFloat(const QString& text)
{
    // _PyUnicode_TransformDecimalAndSpaceToASCII: ASCII kept, Unicode decimal
    // digits (category Nd) mapped to '0'..'9', anything else non-ASCII is
    // invalid. Whitespace is already stripped by the caller, and interior
    // whitespace is invalid either way.
    std::string ascii;
    ascii.reserve(size_t(text.size()));
    bool invalid = false;
    forEachCodePoint(text, [&](char32_t cp) {
        if (invalid)
            return;
        if (cp < 127) {
            ascii.push_back(char(cp));
        } else if (QChar::category(cp) == QChar::Number_DecimalDigit && QChar::digitValue(cp) >= 0) {
            ascii.push_back(char('0' + QChar::digitValue(cp)));
        } else {
            invalid = true;
        }
    });
    if (invalid || ascii.empty())
        return std::nullopt;

    // _Py_string_to_number_with_underscores: '_' only between two digits.
    if (ascii.find('_') != std::string::npos) {
        std::string stripped;
        char prev = '\0';
        for (char c : ascii) {
            const bool prevDigit = prev >= '0' && prev <= '9';
            if (c == '_') {
                if (!prevDigit)
                    return std::nullopt;
            } else {
                if (prev == '_' && !(c >= '0' && c <= '9'))
                    return std::nullopt;
                stripped.push_back(c);
            }
            prev = c;
        }
        if (prev == '_')
            return std::nullopt;
        ascii = stripped;
    }

    // Python float literal grammar (decimal only, no hex). inf/nan spellings
    // are recognised so they map to a non-finite value, like Python.
    static const QRegularExpression numberRe(
        QStringLiteral("^[+-]?(?:\\d+(?:\\.\\d*)?|\\.\\d+)(?:[eE][+-]?\\d+)?$"));
    static const QRegularExpression infNanRe(
        QStringLiteral("^[+-]?(?:inf|infinity|nan)$"),
        QRegularExpression::CaseInsensitiveOption);
    const QString asciiStr = QString::fromLatin1(ascii.data(), qsizetype(ascii.size()));
    if (infNanRe.match(asciiStr).hasMatch())
        return std::numeric_limits<double>::quiet_NaN(); // dropped by caller either way
    if (!numberRe.match(asciiStr).hasMatch())
        return std::nullopt;

    bool negative = false;
    size_t start = 0;
    if (ascii[0] == '+' || ascii[0] == '-') {
        negative = ascii[0] == '-';
        start = 1;
    }
    const char* first = ascii.data() + start;
    const char* last = ascii.data() + ascii.size();
    double value = 0.0;
    const auto res = std::from_chars(first, last, value, std::chars_format::general);
    if (res.ec == std::errc::result_out_of_range) {
        // Overflow -> Python gives inf (dropped). Underflow -> Python gives 0.0.
        // Work out which from the decimal order of magnitude.
        const std::string body(first, last);
        const size_t ePos = body.find_first_of("eE");
        const std::string mant = body.substr(0, ePos);
        long long exp10 = 0;
        if (ePos != std::string::npos) {
            const std::string e = body.substr(ePos + 1);
            bool eNeg = false;
            size_t k = 0;
            if (!e.empty() && (e[0] == '+' || e[0] == '-')) { eNeg = e[0] == '-'; k = 1; }
            for (; k < e.size(); ++k) {
                exp10 = exp10 * 10 + (e[k] - '0');
                if (exp10 > 1000000000LL) { exp10 = 1000000000LL; break; }
            }
            if (eNeg) exp10 = -exp10;
        }
        const size_t dot = mant.find('.');
        const long long intDigits = (long long)(dot == std::string::npos ? mant.size() : dot);
        long long pos = 0, firstNonZero = -1;
        for (char c : mant) {
            if (c == '.') continue;
            if (c != '0') { firstNonZero = pos; break; }
            ++pos;
        }
        if (firstNonZero < 0)
            value = 0.0;
        else if ((intDigits - 1 - firstNonZero) + exp10 > 0)
            value = std::numeric_limits<double>::infinity();
        else
            value = 0.0;
    } else if (res.ec != std::errc() || res.ptr != last) {
        return std::nullopt;
    }
    return negative ? -value : value;
}

// Python repr(float) for a finite double.
QString pyRepr(double d)
{
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), d, std::chars_format::scientific);
    std::string s(buf, res.ptr);
    std::string sign;
    if (!s.empty() && s[0] == '-') {
        sign = "-";
        s.erase(0, 1);
    }
    const size_t ePos = s.find('e');
    const std::string mant = s.substr(0, ePos);
    const int exp = std::stoi(s.substr(ePos + 1));
    std::string digits;
    for (char c : mant)
        if (c != '.')
            digits.push_back(c);
    const int n = int(digits.size());
    const int decpt = exp + 1;

    std::string out;
    if (decpt <= -4 || decpt > 16) {
        out = digits.substr(0, 1);
        if (n > 1)
            out += "." + digits.substr(1);
        const int absExp = exp < 0 ? -exp : exp;
        std::string e = std::to_string(absExp);
        if (e.size() < 2)
            e = "0" + e;
        out += std::string("e") + (exp < 0 ? "-" : "+") + e;
    } else if (decpt <= 0) {
        out = "0." + std::string(size_t(-decpt), '0') + digits;
    } else if (decpt >= n) {
        out = digits + std::string(size_t(decpt - n), '0') + ".0";
    } else {
        out = digits.substr(0, size_t(decpt)) + "." + digits.substr(size_t(decpt));
    }
    return QString::fromLatin1((sign + out).c_str());
}

QString escapeLabelValue(const QString& v)
{
    QString out = v;
    out.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    out.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    out.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return out;
}

QString escapeHelp(const QString& v)
{
    QString out = v;
    out.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    out.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    return out;
}

bool tokenMatch(const QString& value, const QStringList& includes, const QStringList& excludes)
{
    const QString lowered = pyLower(value);
    if (!includes.isEmpty()) {
        bool any = false;
        for (const QString& t : includes)
            if (lowered.contains(t)) { any = true; break; }
        if (!any)
            return false;
    }
    for (const QString& t : excludes)
        if (lowered.contains(t))
            return false;
    return true;
}

bool shouldInclude(const QString& name, const QString& cls, const QString& app, const Options& o)
{
    return tokenMatch(name, o.includeSensors, o.excludeSensors)
        && tokenMatch(cls, o.includeClasses, o.excludeClasses)
        && tokenMatch(app, o.includeApps, o.excludeApps);
}

} // namespace

bool pyIsSpace(char32_t c)
{
    // Py_UNICODE_ISSPACE: the complete set Python's str.strip() removes.
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0
        || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029
        || c == 0x202F || c == 0x205F || c == 0x3000;
}

QString pyStrip(const QString& s)
{
    qsizetype b = 0, e = s.size();
    while (b < e && pyIsSpace(s.at(b).unicode()))
        ++b;
    while (e > b && pyIsSpace(s.at(e - 1).unicode()))
        --e;
    return s.mid(b, e - b);
}

QString pyLower(const QString& s)
{
    // Python lower-cases U+0130 to "i" + U+0307 (full case mapping).
    QString t = s;
    t.replace(QChar(0x0130), QString::fromUtf16(u"i̇"));
    return t.toLower();
}

QString cleanText(const QString& s)
{
    QString text = s;

    // text.encode("latin1", "strict").decode("utf-8", "strict"), kept only if
    // both steps succeed.
    bool latin1 = true;
    for (QChar c : text) {
        if (c.unicode() > 0xFF) {
            latin1 = false;
            break;
        }
    }
    if (latin1) {
        const QByteArray bytes = text.toLatin1();
        if (isStrictUtf8(bytes)) {
            // Stateless: a truncated trailing sequence is an error, not
            // buffered. ConvertInitialBom: keep a leading U+FEFF like Python.
            QStringDecoder decoder(QStringDecoder::Utf8,
                                   QStringDecoder::Flag::Stateless | QStringDecoder::Flag::ConvertInitialBom);
            const QString repaired = decoder.decode(bytes);
            if (!decoder.hasError())
                text = repaired;
        }
    }

    text.replace(QString::fromUtf16(u"Â°C"), QString::fromUtf16(u"°C"));
    text.remove(QChar(u'\0'));
    return pyStrip(text);
}

QString sanitizeName(const QString& s)
{
    QString v = pyLower(pyStrip(s));
    v.replace(QStringLiteral("/"), QStringLiteral("_per_"));
    v.replace(QStringLiteral("+"), QStringLiteral("plus"));
    v.replace(QStringLiteral("#"), QStringLiteral("num"));
    v.replace(QStringLiteral("@"), QStringLiteral("at"));
    v.replace(QStringLiteral("-"), QStringLiteral("_"));
    v.replace(QStringLiteral(" "), QStringLiteral("_"));

    // SAFE_NAME_RE.sub("_", ...) per code point, then collapse "_" runs.
    QString out;
    out.reserve(v.size());
    forEachCodePoint(v, [&](char32_t cp) {
        const bool safe = (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')
                       || (cp >= '0' && cp <= '9') || cp == '_';
        const QChar c = safe ? QChar(char16_t(cp)) : QChar(u'_');
        if (c == u'_' && !out.isEmpty() && out.back() == u'_')
            return;
        out.append(c);
    });
    while (out.startsWith(u'_'))
        out.remove(0, 1);
    while (out.endsWith(u'_'))
        out.chop(1);
    return out.isEmpty() ? QStringLiteral("unknown") : out;
}

QString normalizeUnit(const QString& raw)
{
    const auto& map = unitNormalization();
    const auto it = map.constFind(raw);
    if (it != map.constEnd())
        return it.value();
    return sanitizeName(raw);
}

std::optional<double> safeFloat(const QString& s)
{
    QString text = cleanText(s);
    if (text.isEmpty())
        return std::nullopt;

    // parse_bool_like()
    const QString lowered = pyLower(pyStrip(text));
    if (lowered == u"yes" || lowered == u"true" || lowered == u"on" || lowered == u"enabled")
        return 1.0;
    if (lowered == u"no" || lowered == u"false" || lowered == u"off" || lowered == u"disabled")
        return 0.0;

    text.remove(QLatin1Char(','));
    text = pyStrip(text);
    const auto v = pyFloat(text);
    if (!v || std::isnan(*v) || std::isinf(*v))
        return std::nullopt;
    return v;
}

QStringList parseFilterList(const QString& csv)
{
    QStringList out;
    for (const QString& part : csv.split(QLatin1Char(','))) {
        const QString t = pyStrip(part);
        if (!t.isEmpty())
            out.append(pyLower(t));
    }
    return out;
}

QList<Row> buildRows(const QList<SnapshotRow>& snapshot, const Options& options)
{
    QList<Row> parsed;
    parsed.reserve(snapshot.size());
    for (const SnapshotRow& item : snapshot) {
        Row row;
        row.app = cleanText(item.app);
        row.cls = cleanText(item.cls);
        row.name = cleanText(item.name);
        row.unitRaw = cleanText(item.unit);
        row.unit = normalizeUnit(row.unitRaw);
        row.updateTime = double(item.updateTime);
        const std::optional<double> value = safeFloat(item.valueStr);

        if (row.name.isEmpty())
            continue;
        if (!shouldInclude(row.name, row.cls, row.app, options))
            continue;
        if (!value)
            continue; // Prometheus samples must be numeric.
        row.value = *value;
        parsed.append(row);
    }

    // Occurrence label when several rows share the same label set.
    auto keyOf = [](const Row& r) {
        return QStringList{r.app, r.cls, r.name, r.unit, r.unitRaw};
    };
    QHash<QStringList, int> counts;
    for (const Row& r : parsed)
        ++counts[keyOf(r)];
    QHash<QStringList, int> seen;
    for (Row& r : parsed) {
        const QStringList key = keyOf(r);
        const int n = ++seen[key];
        r.occurrence = counts.value(key) > 1 ? QString::number(n) : QStringLiteral("1");
    }
    return parsed;
}

bool isStale(const Health& health, const Options& options, double nowTs)
{
    return health.lastSuccessTs <= 0 || (nowTs - health.lastSuccessTs) > options.freshnessTimeout;
}

QString formatValue(double d)
{
    // prometheus_client.utils.floatToGoString
    if (std::isinf(d))
        return d > 0 ? QStringLiteral("+Inf") : QStringLiteral("-Inf");
    if (std::isnan(d))
        return QStringLiteral("NaN");
    const QString s = pyRepr(d);
    const qsizetype dot = s.indexOf(QLatin1Char('.'));
    if (d > 0 && dot > 6) {
        QString mantissa = s.left(1) + QLatin1Char('.') + s.mid(1, dot - 1) + s.mid(dot + 1);
        while (!mantissa.isEmpty() && (mantissa.back() == u'0' || mantissa.back() == u'.'))
            mantissa.chop(1);
        return mantissa + QStringLiteral("e+") + QStringLiteral("%1").arg(dot - 1, 2, 10, QLatin1Char('0'));
    }
    return s;
}

QByteArray render(const QList<Row>& rows, const Health& h, const Options& o, double nowTs)
{
    const QString prefix = sanitizeName(o.metricPrefix);
    const bool stale = isStale(h, o, nowTs);
    const double age = h.lastSuccessTs > 0 ? nowTs - h.lastSuccessTs : 1e30;
    const double up = (h.sourceUp && !stale) ? 1.0 : 0.0;

    QString out;
    auto header = [&](const QString& name, const QString& help) {
        out += QStringLiteral("# HELP ") + name + QLatin1Char(' ') + escapeHelp(help) + QLatin1Char('\n');
        out += QStringLiteral("# TYPE ") + name + QStringLiteral(" gauge\n");
    };
    auto gauge = [&](const char* suffix, const char* help, double value) {
        const QString name = prefix + QLatin1String(suffix);
        header(name, QString::fromUtf8(help));
        out += name + QLatin1Char(' ') + formatValue(value) + QLatin1Char('\n');
    };

    gauge("_exporter_up", "1 if source is reachable and data is fresh, else 0", up);
    gauge("_exporter_source_up", "1 if last source poll succeeded, else 0", h.sourceUp ? 1.0 : 0.0);
    gauge("_exporter_stale", "1 if cached data is stale", stale ? 1.0 : 0.0);
    gauge("_exporter_data_age_seconds", "Age of the last successful HWiNFO sample", age);
    gauge("_exporter_freshness_timeout_seconds",
          "Configured freshness timeout; data older than this is not exported", o.freshnessTimeout);
    gauge("_exporter_last_success_timestamp_seconds", "Unix timestamp of the last successful poll",
          h.lastSuccessTs > 0 ? h.lastSuccessTs : 0.0);
    gauge("_exporter_last_attempt_timestamp_seconds", "Unix timestamp of the last poll attempt",
          h.lastAttemptTs > 0 ? h.lastAttemptTs : 0.0);
    gauge("_exporter_last_poll_duration_seconds", "Duration of the last poll", h.lastPollDuration);
    gauge("_exporter_last_http_status", "Last HTTP status from source", double(h.httpStatus));
    gauge("_exporter_successful_polls_total", "Total successful source polls", double(h.ok));
    gauge("_exporter_failed_polls_total", "Total failed source polls", double(h.failed));
    gauge("_exporter_raw_items_last", "Number of raw items in last source payload", double(h.rawItems));
    gauge("_exporter_exported_items_last", "Number of exported numeric items in last successful payload",
          double(h.exportedItems));

    // Stale: no sensor series at all, so Prometheus never scrapes old values
    // as current.
    if (!stale) {
        QStringList labelSets;
        labelSets.reserve(rows.size());
        const QString host = escapeLabelValue(o.hostLabel);
        for (const Row& r : rows) {
            QString l = QStringLiteral("{host=\"") + host
                + QStringLiteral("\",sensor_app=\"") + escapeLabelValue(r.app)
                + QStringLiteral("\",sensor_class=\"") + escapeLabelValue(r.cls)
                + QStringLiteral("\",sensor_name=\"") + escapeLabelValue(r.name)
                + QStringLiteral("\",sensor_unit=\"") + escapeLabelValue(r.unit)
                + QStringLiteral("\",sensor_unit_raw=\"") + escapeLabelValue(r.unitRaw)
                + QStringLiteral("\",occurrence=\"") + escapeLabelValue(r.occurrence)
                + (o.compatSourceLabel ? QStringLiteral("\",source=\"http\"}") : QStringLiteral("\"}"));
            labelSets.append(l);
        }

        auto family = [&](const char* suffix, const char* help, auto valueOf) {
            const QString name = prefix + QLatin1String(suffix);
            header(name, QString::fromUtf8(help));
            for (qsizetype i = 0; i < rows.size(); ++i)
                out += name + labelSets[i] + QLatin1Char(' ') + formatValue(valueOf(rows[i])) + QLatin1Char('\n');
        };
        family("_sensor_value", "Numeric sensor value from HWiNFO", [](const Row& r) { return r.value; });
        family("_sensor_update_time_seconds", "SensorUpdateTime from HWiNFO",
               [](const Row& r) { return r.updateTime; });
        family("_sensor_present", "1 if sensor was present in the most recent fresh payload",
               [](const Row&) { return 1.0; });
    }
    return out.toUtf8();
}

QByteArray renderHealthJson(const Health& h, const Options& o, double nowTs, bool* exporterUp)
{
    const bool stale = isStale(h, o, nowTs);
    const bool up = h.sourceUp && !stale;
    if (exporterUp)
        *exporterUp = up;
    QJsonObject obj;
    obj[QStringLiteral("source_up")] = h.sourceUp;
    obj[QStringLiteral("exporter_up")] = up;
    obj[QStringLiteral("stale")] = stale;
    obj[QStringLiteral("last_success_ts")] = h.lastSuccessTs;
    obj[QStringLiteral("last_attempt_ts")] = h.lastAttemptTs;
    obj[QStringLiteral("last_poll_duration_seconds")] = h.lastPollDuration;
    obj[QStringLiteral("last_http_status")] = h.httpStatus;
    obj[QStringLiteral("last_error")] = h.lastError;
    obj[QStringLiteral("successful_polls")] = qint64(h.ok);
    obj[QStringLiteral("failed_polls")] = qint64(h.failed);
    obj[QStringLiteral("raw_items_last")] = h.rawItems;
    obj[QStringLiteral("exported_items_last")] = h.exportedItems;
    obj[QStringLiteral("data_freshness_timeout_seconds")] = o.freshnessTimeout;
    return QJsonDocument(obj).toJson(QJsonDocument::Indented);
}

Health healthFromPoll(const PollHealth& poll, int exportedRows)
{
    Health h;
    h.sourceUp = poll.lastReadOk;
    h.lastSuccessTs = poll.lastSuccessTs;
    h.lastAttemptTs = poll.lastAttemptTs;
    h.lastPollDuration = poll.lastPollDurationS;
    h.ok = poll.successfulPolls;
    h.failed = poll.failedPolls;
    h.rawItems = poll.lastReadOk ? poll.rawItemsLast : 0;
    h.exportedItems = poll.lastReadOk ? exportedRows : 0;
    h.httpStatus = poll.lastReadOk ? 200 : 0;
    h.lastError = poll.lastError;
    return h;
}

} // namespace prom
