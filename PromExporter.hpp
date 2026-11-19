#pragma once

// Prometheus exposition for MyRsm. A line-by-line port of the output contract
// of DJMalachite/HWINFOtoPrometheusConnector @ 2fb02f5 (hwinfo_prom_exporter.py),
// so existing Grafana dashboards keep working unchanged.
//
// Deliberately free of Windows APIs: it only sees SnapshotRow (the "/" JSON
// route's data) and plain health numbers, so it can be unit-tested and driven
// from JSON fixtures.

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

#include "RsmTypes.hpp"

namespace prom {

inline constexpr char kContentType[] = "text/plain; version=0.0.4; charset=utf-8";
inline constexpr char kHealthContentType[] = "application/json; charset=utf-8";

struct Options {
    QString metricPrefix = QStringLiteral("hwinfo"); // passed through sanitizeName() when rendering
    QString hostLabel;                               // value of the "host" label
    double freshnessTimeout = 20.0;                  // seconds
    // Lower-cased, trimmed, non-empty tokens (see parseFilterList()).
    QStringList includeSensors, excludeSensors;
    QStringList includeClasses, excludeClasses;
    QStringList includeApps, excludeApps;
    bool compatSourceLabel = false;                  // append source="http" (post-MQTT connector)
};

struct Row {
    QString app, cls, name, unit, unitRaw, occurrence;
    double value = 0.0;
    double updateTime = 0.0;
};

struct Health {
    bool sourceUp = false;
    double lastSuccessTs = 0.0;
    double lastAttemptTs = 0.0;
    double lastPollDuration = 0.0;
    quint64 ok = 0;
    quint64 failed = 0;
    int rawItems = 0;
    int exportedItems = 0;
    int httpStatus = 0;
    QString lastError;
};

// --- Python-compatible string helpers -------------------------------------
// Python str.strip() / str.lower() semantics (Unicode whitespace set, U+0130).
bool pyIsSpace(char32_t c);
QString pyStrip(const QString& s);
QString pyLower(const QString& s);

// clean_text(): Latin-1 -> strict UTF-8 mojibake repair, "Â°C" -> "°C",
// drop NULs, strip whitespace.
QString cleanText(const QString& s);
// sanitize_name()
QString sanitizeName(const QString& s);
// normalize_unit() applied to an already clean_text()-ed unit.
QString normalizeUnit(const QString& raw);
// safe_float() for a string value (the "/" route always sends strings).
std::optional<double> safeFloat(const QString& s);
// "a, B ,,c" -> {"a","b","c"} (connector's env-var list parsing).
QStringList parseFilterList(const QString& csv);

// parse_hwinfo_payload(): §1.2 per-row transform + §1.3 occurrence label.
QList<Row> buildRows(const QList<SnapshotRow>& snapshot, const Options& options);

// Prometheus text format 0.0.4, same families/HELP/order as the connector.
bool isStale(const Health& health, const Options& options, double nowTs);
QByteArray render(const QList<Row>& rows, const Health& health, const Options& options, double nowTs);

// /healthz and /readyz body (connector's JSON shape). *exporterUp decides 200 vs 503.
QByteArray renderHealthJson(const Health& health, const Options& options, double nowTs, bool* exporterUp);

// Python repr(float) followed by prometheus_client's floatToGoString().
QString formatValue(double v);

// Converts MyRsm's poll state into exporter health.
Health healthFromPoll(const PollHealth& poll, int exportedRows);

} // namespace prom
