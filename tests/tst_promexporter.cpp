// Unit tests for the Prometheus exporter port (PromExporter) and for the
// "/" JSON route refactor (MyRsm::snapshot / toJson must stay byte-identical).

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QtTest>

#include <cmath>
#include <limits>

#include "MyRsm.hpp"
#include "PromExporter.hpp"

namespace {

// Not QString::fromUtf16(): that treats a leading U+FEFF as a byte-order mark
// and drops it.
QString u16(const char16_t* s) { return QStringView(s).toString(); }

// Verbatim copy of MyRsm::toJson() (and its helpers) as it was before the
// snapshot refactor, operating on explicit data instead of members. The new
// implementation must produce byte-identical JSON.
QString legacySimplifyValue(double value) {
    auto val = (qFuzzyCompare(value, static_cast<int>(value)) ? QString::number(static_cast<int>(value)) : QString::number(value, 'f'));
    if (val.contains('.')) {
        val.remove(QRegularExpression("0+$"));   // remove trailing zeros
        val.remove(QRegularExpression("\\.$"));   // remove trailing dot if all decimals were zeros
    }
    return val;
}

QString legacyGetFromFilters(QList<std::pair<QString, QString>> filters, const QString& key) {
    for (const auto& f : filters) {
        if (f.first == key) {
            return f.second;
        }
    }
    return {};
}

QJsonArray legacyToJson(const QList<MyRsm::Sensor>& m_sensors, const QList<MyRsm::ReadingData>& m_readings,
                        qint64 m_lastPollTime, QList<std::pair<QString, QString>> filters = {})
{
    QRegularExpression classRe;
    bool hasClassFilter = false;
    if (auto pattern = legacyGetFromFilters(filters, "classRegex"); !pattern.isEmpty()) {
        classRe.setPattern(pattern);
        hasClassFilter = true;
    }
    QRegularExpression nameRe;
    bool hasNameFilter = false;
    if (auto pattern = legacyGetFromFilters(filters, "nameRegex"); !pattern.isEmpty()) {
        nameRe.setPattern(pattern);
        hasNameFilter = true;
    }
    const bool includeMinMaxAvg = legacyGetFromFilters(filters, "getMinMaxAvg") == "1";

    QJsonArray arr;
    for (const MyRsm::ReadingData& rd : m_readings) {
        if ((hasClassFilter && !classRe.match(m_sensors[rd.sensorIndex].nameUser).hasMatch()) ||
            (hasNameFilter && !nameRe.match(rd.nameUser).hasMatch()))
            continue;
        QJsonObject obj;
        obj["SensorApp"] = "HWiNFO";
        obj["SensorClass"] = m_sensors[rd.sensorIndex].nameUser;
        obj["SensorName"] = rd.nameUser;
        obj["SensorUnit"] = rd.unit;
        obj["SensorValue"] = legacySimplifyValue(rd.value);
        if (includeMinMaxAvg) {
            obj["SensorValueMin"] = legacySimplifyValue(rd.valueMin);
            obj["SensorValueMax"] = legacySimplifyValue(rd.valueMax);
            obj["SensorValueAvg"] = legacySimplifyValue(rd.valueAvg);
        }
        obj["SensorUpdateTime"] = m_lastPollTime;
        arr.append(obj);
    }
    return arr;
}

QByteArray compact(const QJsonArray& a) { return QJsonDocument(a).toJson(QJsonDocument::Compact); }

QByteArray newToJson(const QList<MyRsm::Sensor>& s, const QList<MyRsm::ReadingData>& r, qint64 t,
                     const MyRsm::Filters& f = {})
{
    bool mma = false;
    for (const auto& p : f)
        if (p.first == "getMinMaxAvg") { mma = p.second == "1"; break; }
    return compact(MyRsm::snapshotToJson(MyRsm::buildSnapshot(s, r, t, f), mma));
}

SnapshotRow snap(const QString& cls, const QString& name, const QString& unit, const QString& value,
                 qint64 t = 1700000000, const QString& app = QStringLiteral("HWiNFO"))
{
    SnapshotRow r;
    r.app = app;
    r.cls = cls;
    r.name = name;
    r.unit = unit;
    r.valueStr = value;
    r.updateTime = t;
    return r;
}

prom::Health freshHealth(double now)
{
    prom::Health h;
    h.sourceUp = true;
    h.lastSuccessTs = now - 1.0;
    h.lastAttemptTs = now - 1.0;
    h.lastPollDuration = 0.005;
    h.ok = 3;
    h.failed = 1;
    h.rawItems = 2;
    h.exportedItems = 2;
    h.httpStatus = 200;
    return h;
}

} // namespace

class TestPromExporter : public QObject
{
    Q_OBJECT
private slots:
    // --- units / names -----------------------------------------------------
    void normalizeUnit_data();
    void normalizeUnit();
    void sanitizeName_data();
    void sanitizeName();
    // --- clean_text ----------------------------------------------------------
    void cleanText_data();
    void cleanText();
    void pyStrip();
    // --- safe_float ----------------------------------------------------------
    void safeFloat_data();
    void safeFloat();
    void parseFilterList();
    // --- rows / occurrence ---------------------------------------------------
    void occurrenceNumbering();
    void occurrenceAfterFilterAndDrops();
    void filtersCaseInsensitiveSubstring();
    // --- render --------------------------------------------------------------
    void formatValue_data();
    void formatValue();
    void renderFresh();
    void renderStaleHasNoSensorFamilies();
    void renderNeverSucceeded();
    void renderLabelEscaping();
    void renderCompatSourceLabel();
    void renderPrefixSanitized();
    void healthJson();
    void healthFromPoll();
    // --- "/" JSON route unchanged ------------------------------------------
    void toJsonGolden();
    void toJsonMatchesLegacy();
};

void TestPromExporter::normalizeUnit_data()
{
    QTest::addColumn<QString>("raw");
    QTest::addColumn<QString>("expected");
    // Every UNIT_NORMALIZATION key.
    QTest::newRow("degC") << u16(u"°C") << "celsius";
    QTest::newRow("C") << "C" << "celsius";
    QTest::newRow("%") << "%" << "percent";
    QTest::newRow("V") << "V" << "volts";
    QTest::newRow("mV") << "mV" << "millivolts";
    QTest::newRow("A") << "A" << "amps";
    QTest::newRow("W") << "W" << "watts";
    QTest::newRow("RPM") << "RPM" << "rpm";
    QTest::newRow("MHz") << "MHz" << "mhz";
    QTest::newRow("GHz") << "GHz" << "ghz";
    QTest::newRow("FPS") << "FPS" << "fps";
    QTest::newRow("fps") << "fps" << "fps";
    QTest::newRow("MB") << "MB" << "megabytes";
    QTest::newRow("GB") << "GB" << "gigabytes";
    QTest::newRow("KB") << "KB" << "kilobytes";
    QTest::newRow("MB/s") << "MB/s" << "megabytes_per_second";
    QTest::newRow("GB/s") << "GB/s" << "gigabytes_per_second";
    QTest::newRow("KB/s") << "KB/s" << "kilobytes_per_second";
    QTest::newRow("ms") << "ms" << "milliseconds";
    QTest::newRow("GT/s") << "GT/s" << "gigatransfers_per_second";
    QTest::newRow("x") << "x" << "ratio";
    QTest::newRow("T") << "T" << "ticks";
    QTest::newRow("Yes/No") << "Yes/No" << "bool";
    QTest::newRow("empty") << "" << "none";
    // Not in the map -> sanitize_name().
    QTest::newRow("degF") << u16(u"°F") << "f";
    QTest::newRow("Wh") << "Wh" << "wh";
    QTest::newRow("mW") << "mW" << "mw";
    QTest::newRow("Gbps") << "Gbps" << "gbps";
    QTest::newRow("w lowercase") << "w" << "w";
    QTest::newRow("mb/s lowercase") << "mb/s" << "mb_per_s";
    QTest::newRow("kWh with euro") << u16(u"€/kWh") << "per_kwh";
}

void TestPromExporter::normalizeUnit()
{
    QFETCH(QString, raw);
    QFETCH(QString, expected);
    QCOMPARE(prom::normalizeUnit(raw), expected);
}

void TestPromExporter::sanitizeName_data()
{
    QTest::addColumn<QString>("in");
    QTest::addColumn<QString>("expected");
    QTest::newRow("symbols") << "  A/B+C#D@E-F G " << "a_per_bpluscnumdate_f_g";
    QTest::newRow("only underscores") << "___" << "unknown";
    QTest::newRow("only symbols") << "__--__" << "unknown";
    QTest::newRow("empty") << "" << "unknown";
    QTest::newRow("whitespace") << " \t " << "unknown";
    QTest::newRow("double slash") << "MB//s" << "mb_per_per_s";
    QTest::newRow("prefix") << "hwinfo" << "hwinfo";
    QTest::newRow("prefix odd") << "  HWiNFO-Shadow #2 " << "hwinfo_shadow_num2";
    QTest::newRow("dotted I") << u16(u"İW") << "i_w";
    QTest::newRow("kelvin sign") << u16(u"K") << "k";
    QTest::newRow("omega") << u16(u"Ω") << "unknown";
    QTest::newRow("emoji") << u16(u"\U0001F600") << "unknown";
    QTest::newRow("emoji between") << u16(u"a\U0001F600b") << "a_b";
    QTest::newRow("micro") << u16(u"µs") << "s";
    QTest::newRow("quote") << "\"" << "unknown";
    QTest::newRow("dots") << "a.b.c" << "a_b_c";
}

void TestPromExporter::sanitizeName()
{
    QFETCH(QString, in);
    QFETCH(QString, expected);
    QCOMPARE(prom::sanitizeName(in), expected);
}

void TestPromExporter::cleanText_data()
{
    QTest::addColumn<QString>("in");
    QTest::addColumn<QString>("expected");
    QTest::newRow("ascii") << "Core Temp" << "Core Temp";
    QTest::newRow("degC kept") << u16(u"°C") << u16(u"°C");
    QTest::newRow("A-circumflex degC repaired") << u16(u"Â°C") << u16(u"°C");
    QTest::newRow("e-acute mojibake") << u16(u"Ã©cran") << u16(u"écran");
    QTest::newRow("latin1 not utf8 kept") << u16(u"Café") << u16(u"Café");
    QTest::newRow("double mojibake one pass") << u16(u"Ã\u0083Â©") << u16(u"Ã©");
    QTest::newRow("mixed >FF no repair") << u16(u"Ã© ☃") << u16(u"Ã© ☃");
    QTest::newRow("embedded NUL") << QString::fromUtf16(u"Fan\u0000 1", 6) << "Fan 1";
    QTest::newRow("trailing NUL then space") << QString::fromUtf16(u"x \u0000", 3) << "x";
    QTest::newRow("whitespace") << "  Padded Name \t\r\n" << "Padded Name";
    QTest::newRow("nbsp") << u16(u" x ") << "x";
    QTest::newRow("em space") << u16(u" x　") << "x";
    QTest::newRow("info separators") << u16(u"\u001Cx\u001F") << "x";
    QTest::newRow("truncated") << u16(u"Ã") << u16(u"Ã");
    QTest::newRow("overlong") << u16(u"À¯") << u16(u"À¯");
    QTest::newRow("encoded surrogate") << u16(u"í \u0080") << u16(u"í \u0080");
    QTest::newRow("above 10FFFF") << u16(u"ô\u0090\u0080\u0080") << u16(u"ô\u0090\u0080\u0080");
    QTest::newRow("4-byte repaired") << u16(u"ð\u009F\u0094¥") << u16(u"\U0001F525");
    QTest::newRow("BOM kept") << u16(u"ï»¿W") << u16(u"﻿W");
    QTest::newRow("nbsp mojibake repaired then stripped") << u16(u"Â 20") << "20";
    QTest::newRow("empty") << "" << "";
}

void TestPromExporter::cleanText()
{
    QFETCH(QString, in);
    QFETCH(QString, expected);
    QCOMPARE(prom::cleanText(in), expected);
}

void TestPromExporter::pyStrip()
{
    QCOMPARE(prom::pyStrip(u16(u"\u0085      a  ")), QStringLiteral("a"));
    // U+200B (zero width space) and U+FEFF are not whitespace in Python.
    QCOMPARE(prom::pyStrip(u16(u"​a﻿")), u16(u"​a﻿"));
}

void TestPromExporter::safeFloat_data()
{
    QTest::addColumn<QString>("in");
    QTest::addColumn<bool>("valid");
    QTest::addColumn<double>("expected");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    QTest::newRow("int") << "45" << true << 45.0;
    QTest::newRow("decimal") << "54.25" << true << 54.25;
    QTest::newRow("negative") << "-273.15" << true << -273.15;
    QTest::newRow("yes") << "Yes" << true << 1.0;
    QTest::newRow("TRUE") << "TRUE" << true << 1.0;
    QTest::newRow("on padded") << " On " << true << 1.0;
    QTest::newRow("enabled") << "Enabled" << true << 1.0;
    QTest::newRow("no") << "No" << true << 0.0;
    QTest::newRow("false") << "false" << true << 0.0;
    QTest::newRow("off") << "off" << true << 0.0;
    QTest::newRow("DISABLED") << "DISABLED" << true << 0.0;
    QTest::newRow("y") << "y" << false << nan;
    QTest::newRow("comma") << "1,234.5" << true << 1234.5;
    QTest::newRow("only comma") << "," << false << nan;
    QTest::newRow("underscore") << "1_000" << true << 1000.0;
    QTest::newRow("underscore exp") << "1.5e1_0" << true << 1.5e10;
    QTest::newRow("double underscore") << "1__000" << false << nan;
    QTest::newRow("leading underscore") << "_1" << false << nan;
    QTest::newRow("trailing underscore") << "1_" << false << nan;
    QTest::newRow("underscore before dot") << "1_.5" << false << nan;
    QTest::newRow("nan") << "nan" << false << nan;
    QTest::newRow("NaN") << "NaN" << false << nan;
    QTest::newRow("inf") << "inf" << false << nan;
    QTest::newRow("-Infinity") << "-Infinity" << false << nan;
    QTest::newRow("overflow") << "1e999" << false << nan;
    QTest::newRow("nan payload") << "nan(123)" << false << nan;
    QTest::newRow("underflow") << "1e-400" << true << 0.0;
    QTest::newRow("denormal") << "5e-324" << true << 5e-324;
    QTest::newRow("max") << "1.7976931348623157e308" << true << 1.7976931348623157e308;
    QTest::newRow("plus") << "+5" << true << 5.0;
    QTest::newRow("leading dot") << ".5" << true << 0.5;
    QTest::newRow("trailing dot") << "5." << true << 5.0;
    QTest::newRow("dot exponent") << "1.e3" << true << 1000.0;
    QTest::newRow("capital E") << "2.5E+2" << true << 250.0;
    QTest::newRow("leading zeros") << "007" << true << 7.0;
    QTest::newRow("arabic-indic") << u16(u"١٢٣.٤") << true << 123.4;
    QTest::newRow("fullwidth") << u16(u"４２") << true << 42.0;
    QTest::newRow("superscript") << u16(u"²") << false << nan;
    QTest::newRow("nbsp padded") << u16(u" 12 ") << true << 12.0;
    QTest::newRow("interior space") << "1 2" << false << nan;
    QTest::newRow("hex") << "0x10" << false << nan;
    QTest::newRow("text") << "abc" << false << nan;
    QTest::newRow("empty") << "" << false << nan;
    QTest::newRow("exponent only") << "e5" << false << nan;
    QTest::newRow("sign only") << "-" << false << nan;
    QTest::newRow("trailing junk") << "12abc" << false << nan;
    QTest::newRow("NUL padded") << QString::fromUtf16(u"1200\u0000", 5) << true << 1200.0;
}

void TestPromExporter::safeFloat()
{
    QFETCH(QString, in);
    QFETCH(bool, valid);
    QFETCH(double, expected);
    const auto v = prom::safeFloat(in);
    QCOMPARE(v.has_value(), valid);
    if (valid)
        QCOMPARE(*v, expected);
}

void TestPromExporter::parseFilterList()
{
    QCOMPARE(prom::parseFilterList(" GPU, ,Clock ,,"), (QStringList{"gpu", "clock"}));
    QCOMPARE(prom::parseFilterList(""), QStringList{});
}

void TestPromExporter::occurrenceNumbering()
{
    const QList<SnapshotRow> in = {
        snap("CPU", "Core Clock", "MHz", "4700"),
        snap("GPU", "Fan", "RPM", "900"),
        snap("CPU", "Core Clock", "MHz", "4650"),
        snap("CPU", "Core Clock", "MHz", "800"),
        snap("GPU", "Fan", "RPM", "910"),
        snap("CPU", "Unique", "MHz", "1"),
        // Same name/unit_raw after cleaning -> same group.
        snap("DIMM", "Temp", u16(u"°C"), "40"),
        snap("DIMM", "Temp", u16(u"Â°C"), "41"),
        // Different unit_raw (C vs °C) -> different group, though both are "celsius".
        snap("DIMM", "Temp", "C", "42"),
    };
    const auto rows = prom::buildRows(in, prom::Options{});
    QCOMPARE(rows.size(), 9);
    const QStringList occ = [&] { QStringList l; for (const auto& r : rows) l << r.occurrence; return l; }();
    QCOMPARE(occ, (QStringList{"1", "1", "2", "3", "2", "1", "1", "2", "1"}));
    QCOMPARE(rows[8].unit, QStringLiteral("celsius"));
    QCOMPARE(rows[8].unitRaw, QStringLiteral("C"));
    QCOMPARE(rows[7].unitRaw, u16(u"°C"));
}

void TestPromExporter::occurrenceAfterFilterAndDrops()
{
    const QList<SnapshotRow> in = {
        snap("CPU", "Core Clock", "MHz", "4700"),
        snap("GPU", "Core Clock", "MHz", "1"),
        snap("CPU", "Core Clock", "MHz", "n/a"),    // dropped: not numeric
        snap("GPU", "Core Clock", "MHz", "2"),
        snap("CPU", "Core Clock", "MHz", "4650"),
        snap("CPU", "Bus Clock", "MHz", "100"),
        snap("CPU", "Bus Clock", "MHz", "abc"),     // dropped -> remaining one is "1"
        snap("CPU", "", "MHz", "5"),                // dropped: empty name
    };
    prom::Options o;
    o.excludeClasses = prom::parseFilterList("gpu");
    const auto rows = prom::buildRows(in, o);
    QCOMPARE(rows.size(), 3);
    QCOMPARE(rows[0].value, 4700.0);
    QCOMPARE(rows[0].occurrence, QStringLiteral("1"));
    QCOMPARE(rows[1].value, 4650.0);
    QCOMPARE(rows[1].occurrence, QStringLiteral("2"));
    QCOMPARE(rows[2].name, QStringLiteral("Bus Clock"));
    QCOMPARE(rows[2].occurrence, QStringLiteral("1"));

    // Without the filter the GPU group is numbered on its own.
    const auto all = prom::buildRows(in, prom::Options{});
    QCOMPARE(all.size(), 5);
    QCOMPARE(all[1].cls, QStringLiteral("GPU"));
    QCOMPARE(all[1].occurrence, QStringLiteral("1"));
    QCOMPARE(all[2].occurrence, QStringLiteral("2"));
}

void TestPromExporter::filtersCaseInsensitiveSubstring()
{
    const QList<SnapshotRow> in = {
        snap("CPU [#0]", "Core 0 Clock", "MHz", "1"),
        snap("GPU [#0]", "GPU Clock", "MHz", "2"),
        snap("GPU [#0]", "GPU Temperature", u16(u"°C"), "3"),
        snap("CPU [#0]", "Core 0 Clock", "MHz", "4", 1, "AIDA64"),
    };
    prom::Options o;
    o.includeSensors = prom::parseFilterList("CLOCK");
    o.excludeApps = prom::parseFilterList("aida");
    const auto rows = prom::buildRows(in, o);
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0].value, 1.0);
    QCOMPARE(rows[1].value, 2.0);

    prom::Options o2;
    o2.includeClasses = prom::parseFilterList("gpu");
    o2.excludeSensors = prom::parseFilterList("temp");
    const auto rows2 = prom::buildRows(in, o2);
    QCOMPARE(rows2.size(), 1);
    QCOMPARE(rows2[0].name, QStringLiteral("GPU Clock"));
}

void TestPromExporter::formatValue_data()
{
    QTest::addColumn<double>("v");
    QTest::addColumn<QString>("expected");
    QTest::newRow("1") << 1.0 << "1.0";
    QTest::newRow("0") << 0.0 << "0.0";
    QTest::newRow("-0") << -0.0 << "-0.0";
    QTest::newRow("1e30") << 1e30 << "1e+30";
    QTest::newRow("now") << 1700000000.0 << "1.7e+09";
    QTest::newRow("now-1") << 1699999999.0 << "1.699999999e+09";
    QTest::newRow("0.005") << 0.005 << "0.005";
    QTest::newRow("1e-05") << 1e-5 << "1e-05";
    QTest::newRow("0.0001") << 0.0001 << "0.0001";
    QTest::newRow("million") << 1000000.0 << "1e+06";
    QTest::newRow("123456") << 123456.0 << "123456.0";
    QTest::newRow("1234567.5") << 1234567.5 << "1.2345675e+06";
    QTest::newRow("1e15") << 1e15 << "1e+15";
    QTest::newRow("1e16") << 1e16 << "1e+16";
    QTest::newRow("-1e16") << -1e16 << "-1e+16";
    QTest::newRow("-1234567.5") << -1234567.5 << "-1234567.5";
    QTest::newRow("0.1") << 0.1 << "0.1";
    QTest::newRow("54.25") << 54.25 << "54.25";
    QTest::newRow("200") << 200.0 << "200.0";
    QTest::newRow("5e-324") << 5e-324 << "5e-324";
    QTest::newRow("max") << 1.7976931348623157e308 << "1.7976931348623157e+308";
    QTest::newRow("inf") << std::numeric_limits<double>::infinity() << "+Inf";
    QTest::newRow("-inf") << -std::numeric_limits<double>::infinity() << "-Inf";
    QTest::newRow("nan") << std::numeric_limits<double>::quiet_NaN() << "NaN";
}

void TestPromExporter::formatValue()
{
    QFETCH(double, v);
    QFETCH(QString, expected);
    QCOMPARE(prom::formatValue(v), expected);
}

void TestPromExporter::renderFresh()
{
    const double now = 1700000000.0;
    prom::Options o;
    o.hostLabel = "pc";
    const auto rows = prom::buildRows({snap("CPU", "Core Temp", u16(u"°C"), "54.25"),
                                       snap("CPU", "Usage", "%", "7")}, o);
    const QString text = QString::fromUtf8(prom::render(rows, freshHealth(now), o, now));
    const QString expected = QString::fromUtf8(
        "# HELP hwinfo_exporter_up 1 if source is reachable and data is fresh, else 0\n"
        "# TYPE hwinfo_exporter_up gauge\n"
        "hwinfo_exporter_up 1.0\n"
        "# HELP hwinfo_exporter_source_up 1 if last source poll succeeded, else 0\n"
        "# TYPE hwinfo_exporter_source_up gauge\n"
        "hwinfo_exporter_source_up 1.0\n"
        "# HELP hwinfo_exporter_stale 1 if cached data is stale\n"
        "# TYPE hwinfo_exporter_stale gauge\n"
        "hwinfo_exporter_stale 0.0\n"
        "# HELP hwinfo_exporter_data_age_seconds Age of the last successful HWiNFO sample\n"
        "# TYPE hwinfo_exporter_data_age_seconds gauge\n"
        "hwinfo_exporter_data_age_seconds 1.0\n"
        "# HELP hwinfo_exporter_freshness_timeout_seconds Configured freshness timeout; data older than this is not exported\n"
        "# TYPE hwinfo_exporter_freshness_timeout_seconds gauge\n"
        "hwinfo_exporter_freshness_timeout_seconds 20.0\n"
        "# HELP hwinfo_exporter_last_success_timestamp_seconds Unix timestamp of the last successful poll\n"
        "# TYPE hwinfo_exporter_last_success_timestamp_seconds gauge\n"
        "hwinfo_exporter_last_success_timestamp_seconds 1.699999999e+09\n"
        "# HELP hwinfo_exporter_last_attempt_timestamp_seconds Unix timestamp of the last poll attempt\n"
        "# TYPE hwinfo_exporter_last_attempt_timestamp_seconds gauge\n"
        "hwinfo_exporter_last_attempt_timestamp_seconds 1.699999999e+09\n"
        "# HELP hwinfo_exporter_last_poll_duration_seconds Duration of the last poll\n"
        "# TYPE hwinfo_exporter_last_poll_duration_seconds gauge\n"
        "hwinfo_exporter_last_poll_duration_seconds 0.005\n"
        "# HELP hwinfo_exporter_last_http_status Last HTTP status from source\n"
        "# TYPE hwinfo_exporter_last_http_status gauge\n"
        "hwinfo_exporter_last_http_status 200.0\n"
        "# HELP hwinfo_exporter_successful_polls_total Total successful source polls\n"
        "# TYPE hwinfo_exporter_successful_polls_total gauge\n"
        "hwinfo_exporter_successful_polls_total 3.0\n"
        "# HELP hwinfo_exporter_failed_polls_total Total failed source polls\n"
        "# TYPE hwinfo_exporter_failed_polls_total gauge\n"
        "hwinfo_exporter_failed_polls_total 1.0\n"
        "# HELP hwinfo_exporter_raw_items_last Number of raw items in last source payload\n"
        "# TYPE hwinfo_exporter_raw_items_last gauge\n"
        "hwinfo_exporter_raw_items_last 2.0\n"
        "# HELP hwinfo_exporter_exported_items_last Number of exported numeric items in last successful payload\n"
        "# TYPE hwinfo_exporter_exported_items_last gauge\n"
        "hwinfo_exporter_exported_items_last 2.0\n"
        "# HELP hwinfo_sensor_value Numeric sensor value from HWiNFO\n"
        "# TYPE hwinfo_sensor_value gauge\n"
        "hwinfo_sensor_value{host=\"pc\",sensor_app=\"HWiNFO\",sensor_class=\"CPU\",sensor_name=\"Core Temp\",sensor_unit=\"celsius\",sensor_unit_raw=\"°C\",occurrence=\"1\"} 54.25\n"
        "hwinfo_sensor_value{host=\"pc\",sensor_app=\"HWiNFO\",sensor_class=\"CPU\",sensor_name=\"Usage\",sensor_unit=\"percent\",sensor_unit_raw=\"%\",occurrence=\"1\"} 7.0\n"
        "# HELP hwinfo_sensor_update_time_seconds SensorUpdateTime from HWiNFO\n"
        "# TYPE hwinfo_sensor_update_time_seconds gauge\n"
        "hwinfo_sensor_update_time_seconds{host=\"pc\",sensor_app=\"HWiNFO\",sensor_class=\"CPU\",sensor_name=\"Core Temp\",sensor_unit=\"celsius\",sensor_unit_raw=\"°C\",occurrence=\"1\"} 1.7e+09\n"
        "hwinfo_sensor_update_time_seconds{host=\"pc\",sensor_app=\"HWiNFO\",sensor_class=\"CPU\",sensor_name=\"Usage\",sensor_unit=\"percent\",sensor_unit_raw=\"%\",occurrence=\"1\"} 1.7e+09\n"
        "# HELP hwinfo_sensor_present 1 if sensor was present in the most recent fresh payload\n"
        "# TYPE hwinfo_sensor_present gauge\n"
        "hwinfo_sensor_present{host=\"pc\",sensor_app=\"HWiNFO\",sensor_class=\"CPU\",sensor_name=\"Core Temp\",sensor_unit=\"celsius\",sensor_unit_raw=\"°C\",occurrence=\"1\"} 1.0\n"
        "hwinfo_sensor_present{host=\"pc\",sensor_app=\"HWiNFO\",sensor_class=\"CPU\",sensor_name=\"Usage\",sensor_unit=\"percent\",sensor_unit_raw=\"%\",occurrence=\"1\"} 1.0\n");
    QCOMPARE(text, expected);
}

void TestPromExporter::renderStaleHasNoSensorFamilies()
{
    const double now = 1700000000.0;
    prom::Options o;
    o.hostLabel = "pc";
    const auto rows = prom::buildRows({snap("CPU", "Core Temp", "C", "54")}, o);
    prom::Health h = freshHealth(now);
    h.lastSuccessTs = now - 20.5; // > 20 s timeout
    const QString text = QString::fromUtf8(prom::render(rows, h, o, now));
    QVERIFY(text.contains("hwinfo_exporter_stale 1.0\n"));
    QVERIFY(text.contains("hwinfo_exporter_up 0.0\n"));
    QVERIFY(text.contains("hwinfo_exporter_source_up 1.0\n"));
    QVERIFY(!text.contains("_sensor_"));
    QCOMPARE(text.count("# TYPE "), 13);

    // Exactly at the timeout is still fresh (strictly greater-than).
    h.lastSuccessTs = now - 20.0;
    QVERIFY(QString::fromUtf8(prom::render(rows, h, o, now)).contains("hwinfo_exporter_stale 0.0\n"));
}

void TestPromExporter::renderNeverSucceeded()
{
    prom::Options o;
    prom::Health h; // never succeeded
    h.lastAttemptTs = 1699999999.0;
    h.failed = 4;
    const QString text = QString::fromUtf8(prom::render({}, h, o, 1700000000.0));
    QVERIFY(text.contains("hwinfo_exporter_data_age_seconds 1e+30\n"));
    QVERIFY(text.contains("hwinfo_exporter_stale 1.0\n"));
    QVERIFY(text.contains("hwinfo_exporter_up 0.0\n"));
    QVERIFY(text.contains("hwinfo_exporter_last_success_timestamp_seconds 0.0\n"));
    QVERIFY(text.contains("hwinfo_exporter_last_http_status 0.0\n"));
    QVERIFY(text.contains("hwinfo_exporter_failed_polls_total 4.0\n"));
    QVERIFY(!text.contains("_sensor_"));
}

void TestPromExporter::renderLabelEscaping()
{
    const double now = 1700000000.0;
    prom::Options o;
    o.hostLabel = "h\\o\"s\nt";
    const auto rows = prom::buildRows({snap("C\\l\"s", "Line1\nLine2", "\\", "1")}, o);
    const QString text = QString::fromUtf8(prom::render(rows, freshHealth(now), o, now));
    QVERIFY2(text.contains(
        "hwinfo_sensor_value{host=\"h\\\\o\\\"s\\nt\",sensor_app=\"HWiNFO\",sensor_class=\"C\\\\l\\\"s\","
        "sensor_name=\"Line1\\nLine2\",sensor_unit=\"unknown\",sensor_unit_raw=\"\\\\\",occurrence=\"1\"} 1.0\n"),
        qPrintable(text));
    // Every sample stays on one line.
    for (const QString& line : text.split('\n', Qt::SkipEmptyParts))
        QVERIFY2(line.startsWith("# ") || line.startsWith("hwinfo_"), qPrintable(line));
}

void TestPromExporter::renderCompatSourceLabel()
{
    const double now = 1700000000.0;
    prom::Options o;
    o.hostLabel = "pc";
    o.compatSourceLabel = true;
    const auto rows = prom::buildRows({snap("CPU", "Usage", "%", "7")}, o);
    const QString text = QString::fromUtf8(prom::render(rows, freshHealth(now), o, now));
    QVERIFY(text.contains("occurrence=\"1\",source=\"http\"} 7.0\n"));
    QCOMPARE(text.count("source=\"http\""), 3);
}

void TestPromExporter::renderPrefixSanitized()
{
    const double now = 1700000000.0;
    prom::Options o;
    o.metricPrefix = "HWiNFO Shadow";
    const QString text = QString::fromUtf8(prom::render({}, freshHealth(now), o, now));
    QVERIFY(text.startsWith("# HELP hwinfo_shadow_exporter_up "));
    QVERIFY(!text.contains("\nhwinfo_exporter_"));
}

void TestPromExporter::healthJson()
{
    const double now = 1700000000.0;
    prom::Options o;
    bool up = false;
    const QJsonObject fresh = QJsonDocument::fromJson(prom::renderHealthJson(freshHealth(now), o, now, &up)).object();
    QVERIFY(up);
    const QStringList keys = {"source_up", "exporter_up", "stale", "last_success_ts", "last_attempt_ts",
                              "last_poll_duration_seconds", "last_http_status", "last_error", "successful_polls",
                              "failed_polls", "raw_items_last", "exported_items_last",
                              "data_freshness_timeout_seconds"};
    QCOMPARE(fresh.keys().size(), keys.size());
    for (const QString& k : keys)
        QVERIFY2(fresh.contains(k), qPrintable(k));
    QCOMPARE(fresh["exporter_up"].toBool(), true);
    QCOMPARE(fresh["stale"].toBool(), false);
    QCOMPARE(fresh["last_http_status"].toInt(), 200);
    QCOMPARE(fresh["data_freshness_timeout_seconds"].toDouble(), 20.0);

    prom::Health stale = freshHealth(now);
    stale.lastSuccessTs = now - 100;
    const QJsonObject s = QJsonDocument::fromJson(prom::renderHealthJson(stale, o, now, &up)).object();
    QVERIFY(!up);
    QCOMPARE(s["stale"].toBool(), true);
    QCOMPARE(s["source_up"].toBool(), true);
    QCOMPARE(s["exporter_up"].toBool(), false);
}

void TestPromExporter::healthFromPoll()
{
    PollHealth p;
    p.lastReadOk = true;
    p.lastSuccessTs = 10;
    p.lastAttemptTs = 11;
    p.lastPollDurationS = 0.25;
    p.successfulPolls = 5;
    p.failedPolls = 2;
    p.rawItemsLast = 300;
    auto h = prom::healthFromPoll(p, 280);
    QVERIFY(h.sourceUp);
    QCOMPARE(h.httpStatus, 200);
    QCOMPARE(h.rawItems, 300);
    QCOMPARE(h.exportedItems, 280);
    QCOMPARE(h.ok, quint64(5));
    QCOMPARE(h.failed, quint64(2));

    p.lastReadOk = false;
    p.lastError = "boom";
    h = prom::healthFromPoll(p, 280);
    QVERIFY(!h.sourceUp);
    QCOMPARE(h.httpStatus, 0);
    QCOMPARE(h.exportedItems, 0);
    QCOMPARE(h.rawItems, 0);
    QCOMPARE(h.lastError, QStringLiteral("boom"));
}

void TestPromExporter::toJsonGolden()
{
    const QList<MyRsm::Sensor> sensors = {{"CPU", "CPU [#0]"}, {"GPU", "GPU [#0]"}};
    const QList<MyRsm::ReadingData> readings = {
        {0, "Core Temp", "Core Temp", "C", 45.0, 30.0, 70.0, 44.5},
        {1, "GPU Clock", "GPU Clock", "MHz", 1234.5, 210.0, 1905.0, 900.25},
    };
    const QByteArray golden =
        "[{\"SensorApp\":\"HWiNFO\",\"SensorClass\":\"CPU [#0]\",\"SensorName\":\"Core Temp\",\"SensorUnit\":\"C\","
        "\"SensorUpdateTime\":1700000000,\"SensorValue\":\"45\"},"
        "{\"SensorApp\":\"HWiNFO\",\"SensorClass\":\"GPU [#0]\",\"SensorName\":\"GPU Clock\",\"SensorUnit\":\"MHz\","
        "\"SensorUpdateTime\":1700000000,\"SensorValue\":\"1234.5\"}]";
    QCOMPARE(newToJson(sensors, readings, 1700000000), golden);
    QCOMPARE(compact(legacyToJson(sensors, readings, 1700000000)), golden);

    const QByteArray goldenMma =
        "[{\"SensorApp\":\"HWiNFO\",\"SensorClass\":\"GPU [#0]\",\"SensorName\":\"GPU Clock\",\"SensorUnit\":\"MHz\","
        "\"SensorUpdateTime\":1700000000,\"SensorValue\":\"1234.5\",\"SensorValueAvg\":\"900.25\","
        "\"SensorValueMax\":\"1905\",\"SensorValueMin\":\"210\"}]";
    const MyRsm::Filters f = {{"classRegex", "GPU"}, {"getMinMaxAvg", "1"}};
    QCOMPARE(newToJson(sensors, readings, 1700000000, f), goldenMma);
    QCOMPARE(compact(legacyToJson(sensors, readings, 1700000000, f)), goldenMma);
}

void TestPromExporter::toJsonMatchesLegacy()
{
    const QList<MyRsm::Sensor> sensors = {
        {"CPU", "CPU [#0]: AMD Ryzen 9 5900X: Enhanced"},
        {"GPU", u16(u"GPU [#0]: °C café \"q\" \\ \U0001F600")},
        {"Drive", "S.M.A.R.T.: Samsung"},
    };
    const QList<MyRsm::ReadingData> readings = {
        {0, "a", "Core Temp", u16(u"°C"), 54.25, 30.0, 80.125, 50.0},
        {0, "b", "Core Clock", "MHz", 4549.6, 0.0, 4950.0, 3000.000001},
        {1, "c", "Line1\nLine2", "W", -0.5, -1.0, 0.0, -0.25},
        {1, "d", "Zero", "%", 0.0, 0.0, 0.0, 0.0},
        {1, "e", "Big", "MB", 2147483647.0, 1e9, 2.5e9, 123456789.123456},
        {2, "f", "Tiny", "V", 0.0000004, 0.0000005, 1e-7, 0.1234567},
        {2, "g", u16(u"Nul space"), "", 1.0, 1.0, 1.0, 1.0},
        {2, "h", "Temp", u16(u"°C"), 39.0, 38.0, 41.0, 39.5},
    };
    const QList<MyRsm::Filters> cases = {
        {},
        {{"getMinMaxAvg", "1"}},
        {{"getMinMaxAvg", "0"}},
        {{"classRegex", ".*GPU.*"}},
        {{"nameRegex", "^Core"}},
        {{"classRegex", "CPU|Drive"}, {"nameRegex", "Temp"}, {"getMinMaxAvg", "1"}},
        {{"classRegex", "("}},          // invalid regex: matches nothing
        {{"classRegex", ""}},           // empty = no filter
        {{"unknown", "x"}},
    };
    for (const auto& f : cases) {
        const QByteArray legacy = compact(legacyToJson(sensors, readings, 1763560000, f));
        QCOMPARE(newToJson(sensors, readings, 1763560000, f), legacy);
    }
    QCOMPARE(newToJson({}, {}, 0), QByteArray("[]"));
}

QTEST_GUILESS_MAIN(TestPromExporter)
#include "tst_promexporter.moc"
