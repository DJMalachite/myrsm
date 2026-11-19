#include "Config.hpp"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QSysInfo>

#include <cmath>

namespace {

const char kGroup[] = "myrsm";

std::optional<quint16> parsePort(const QString& s)
{
    bool ok = false;
    const uint v = s.trimmed().toUInt(&ok);
    if (!ok || v < 1 || v > 65535)
        return std::nullopt;
    return quint16(v);
}

std::optional<int> parsePollMs(const QString& s, QStringList* warnings, const QString& origin)
{
    bool ok = false;
    int v = s.trimmed().toInt(&ok);
    if (!ok)
        return std::nullopt;
    if (v < kMinPollIntervalMs) {
        warnings->append(QStringLiteral("%1: poll interval %2 ms is below the %3 ms minimum; clamping.")
                             .arg(origin, QString::number(v), QString::number(kMinPollIntervalMs)));
        v = kMinPollIntervalMs;
    }
    return v;
}

std::optional<double> parseTimeout(const QString& s)
{
    bool ok = false;
    const double v = s.trimmed().toDouble(&ok);
    if (!ok || !(v > 0) || !std::isfinite(v))
        return std::nullopt;
    return v;
}

std::optional<bool> parseBool(const QString& s)
{
    const QString v = s.trimmed().toLower();
    if (v == u"1" || v == u"true" || v == u"yes" || v == u"on")
        return true;
    if (v == u"0" || v == u"false" || v == u"no" || v == u"off" || v.isEmpty())
        return false;
    return std::nullopt;
}

std::optional<QHostAddress> parseBindAddress(const QString& s)
{
    const QString v = s.trimmed();
    if (v.isEmpty() || v.compare(u"any", Qt::CaseInsensitive) == 0)
        return QHostAddress(QHostAddress::Any);
    QHostAddress a;
    if (!a.setAddress(v))
        return std::nullopt;
    return a;
}

// QSettings' INI parser turns an unquoted value containing commas into a
// QStringList; join it back so "gpu, cpu" stays a comma-separated list.
QString iniString(const QVariant& v)
{
    if (v.typeId() == QMetaType::QStringList)
        return v.toStringList().join(QLatin1Char(','));
    return v.toString();
}

QString invalid(const QString& origin, const QString& key, const QString& value)
{
    return QStringLiteral("%1: ignoring invalid %2=\"%3\"").arg(origin, key, value);
}

} // namespace

QString defaultConfigPath()
{
    const QString programData = qEnvironmentVariable("ProgramData", QStringLiteral("C:\\ProgramData"));
    return QDir::toNativeSeparators(programData + QStringLiteral("/MyRsm/myrsm.ini"));
}

void addConfigOptions(QCommandLineParser& parser)
{
    parser.addOptions({
        {"config", QStringLiteral("INI file to read (default %1).").arg(defaultConfigPath()), "file"},
        {"httpport", "The port the http server listens on (default 34567).", "httpport"},
        {"wsport", "The port the websocket server listens on (default 34568).", "wsport"},
        {"pollms", "Shared memory poll interval in milliseconds (min 50, default 250).", "pollms"},
        {"bind-address", "Address the HTTP and WebSocket servers listen on (default: all interfaces).", "address"},
        {"metric-prefix", "Prometheus metric name prefix (default hwinfo).", "prefix"},
        {"host-label", "Value of the host label on /metrics (default %COMPUTERNAME%).", "name"},
        {"freshness-timeout", "Seconds without a fresh HWiNFO sample before /metrics drops sensor series (default 20).",
         "seconds"},
        {"include-sensors", "Comma-separated, case-insensitive substrings; only matching sensor names are exported.", "list"},
        {"exclude-sensors", "Comma-separated, case-insensitive substrings; matching sensor names are not exported.", "list"},
        {"include-classes", "Comma-separated, case-insensitive substrings; only matching sensor classes are exported.", "list"},
        {"exclude-classes", "Comma-separated, case-insensitive substrings; matching sensor classes are not exported.", "list"},
        {"include-apps", "Comma-separated, case-insensitive substrings; only matching sensor apps are exported.", "list"},
        {"exclude-apps", "Comma-separated, case-insensitive substrings; matching sensor apps are not exported.", "list"},
        {"compat-source-label",
         "Append source=\"http\" to sensor series (matches the post-MQTT connector's extra label)."},
    });
}

void applyIniFile(const QString& path, AppConfig* c, QStringList* warnings)
{
    c->configPath = path;
    c->configLoaded = false;
    if (!QFileInfo(path).isFile())
        return;

    QSettings s(path, QSettings::IniFormat);
    if (s.status() != QSettings::NoError) {
        warnings->append(QStringLiteral("%1: could not be parsed; using defaults.").arg(path));
        return;
    }
    c->configLoaded = true;

    for (const QString& k : s.childKeys())
        warnings->append(QStringLiteral("%1: key \"%2\" is outside the [myrsm] section; ignored.").arg(path, k));
    for (const QString& g : s.childGroups())
        if (g.compare(QLatin1String(kGroup), Qt::CaseInsensitive) != 0)
            warnings->append(QStringLiteral("%1: section [%2] is not used; ignored.").arg(path, g));

    s.beginGroup(QLatin1String(kGroup));
    for (const QString& rawKey : s.childKeys()) {
        const QString key = rawKey.toLower();
        const QString value = iniString(s.value(rawKey)).trimmed();
        if (key == u"http_port") {
            if (auto p = parsePort(value)) c->httpPort = *p; else warnings->append(invalid(path, key, value));
        } else if (key == u"ws_port") {
            if (auto p = parsePort(value)) c->wsPort = *p; else warnings->append(invalid(path, key, value));
        } else if (key == u"poll_ms") {
            if (auto p = parsePollMs(value, warnings, path)) c->pollMs = *p; else warnings->append(invalid(path, key, value));
        } else if (key == u"bind_address") {
            if (auto a = parseBindAddress(value)) c->bindAddress = *a; else warnings->append(invalid(path, key, value));
        } else if (key == u"metric_prefix") {
            if (!value.isEmpty()) c->prom.metricPrefix = value;
        } else if (key == u"host_label") {
            if (!value.isEmpty()) c->prom.hostLabel = value;
        } else if (key == u"freshness_timeout") {
            if (auto t = parseTimeout(value)) c->prom.freshnessTimeout = *t; else warnings->append(invalid(path, key, value));
        } else if (key == u"compat_source_label") {
            if (auto b = parseBool(value)) c->prom.compatSourceLabel = *b; else warnings->append(invalid(path, key, value));
        } else if (key == u"include_sensors") {
            c->prom.includeSensors = prom::parseFilterList(value);
        } else if (key == u"exclude_sensors") {
            c->prom.excludeSensors = prom::parseFilterList(value);
        } else if (key == u"include_classes") {
            c->prom.includeClasses = prom::parseFilterList(value);
        } else if (key == u"exclude_classes") {
            c->prom.excludeClasses = prom::parseFilterList(value);
        } else if (key == u"include_apps") {
            c->prom.includeApps = prom::parseFilterList(value);
        } else if (key == u"exclude_apps") {
            c->prom.excludeApps = prom::parseFilterList(value);
        } else {
            warnings->append(QStringLiteral("%1: unknown key \"%2\"; ignored.").arg(path, rawKey));
        }
    }
    s.endGroup();
}

void applyCommandLine(const QCommandLineParser& p, AppConfig* c, QStringList* warnings)
{
    const QString origin = QStringLiteral("command line");
    auto bad = [&](const char* opt) {
        warnings->append(QStringLiteral("%1: ignoring invalid --%2 \"%3\"")
                             .arg(origin, QLatin1String(opt), p.value(QLatin1String(opt))));
    };
    if (p.isSet("httpport")) {
        if (auto v = parsePort(p.value("httpport"))) c->httpPort = *v; else bad("httpport");
    }
    if (p.isSet("wsport")) {
        if (auto v = parsePort(p.value("wsport"))) c->wsPort = *v; else bad("wsport");
    }
    if (p.isSet("pollms")) {
        if (auto v = parsePollMs(p.value("pollms"), warnings, origin)) c->pollMs = *v; else bad("pollms");
    }
    if (p.isSet("bind-address")) {
        if (auto v = parseBindAddress(p.value("bind-address"))) c->bindAddress = *v; else bad("bind-address");
    }
    if (p.isSet("metric-prefix"))
        c->prom.metricPrefix = p.value("metric-prefix");
    if (p.isSet("host-label"))
        c->prom.hostLabel = p.value("host-label");
    if (p.isSet("freshness-timeout")) {
        if (auto v = parseTimeout(p.value("freshness-timeout"))) c->prom.freshnessTimeout = *v; else bad("freshness-timeout");
    }
    if (p.isSet("include-sensors")) c->prom.includeSensors = prom::parseFilterList(p.value("include-sensors"));
    if (p.isSet("exclude-sensors")) c->prom.excludeSensors = prom::parseFilterList(p.value("exclude-sensors"));
    if (p.isSet("include-classes")) c->prom.includeClasses = prom::parseFilterList(p.value("include-classes"));
    if (p.isSet("exclude-classes")) c->prom.excludeClasses = prom::parseFilterList(p.value("exclude-classes"));
    if (p.isSet("include-apps")) c->prom.includeApps = prom::parseFilterList(p.value("include-apps"));
    if (p.isSet("exclude-apps")) c->prom.excludeApps = prom::parseFilterList(p.value("exclude-apps"));
    if (p.isSet("compat-source-label"))
        c->prom.compatSourceLabel = true;
}

AppConfig resolveConfig(const QCommandLineParser& parser, QStringList* warnings)
{
    AppConfig c;
    // prom::Options defaults: prefix hwinfo, timeout 20, no filters, no source label.
    c.prom.hostLabel.clear();

    const QString path = parser.isSet("config") ? parser.value("config") : defaultConfigPath();
    if (parser.isSet("config") && !QFileInfo(path).isFile())
        warnings->append(QStringLiteral("--config %1: file not found; using defaults.").arg(path));
    applyIniFile(path, &c, warnings);
    applyCommandLine(parser, &c, warnings);

    if (c.httpPort == c.wsPort)
        warnings->append(QStringLiteral("HTTP and WebSocket ports are both %1; the WebSocket server will fail to start.")
                             .arg(c.httpPort));

    // Default host label: only when neither the ini nor the command line set one
    // (an explicit --host-label "" is honoured).
    if (c.prom.hostLabel.isEmpty() && !parser.isSet("host-label")) {
        c.prom.hostLabel = qEnvironmentVariable("COMPUTERNAME");
        if (c.prom.hostLabel.isEmpty())
            c.prom.hostLabel = QSysInfo::machineHostName();
    }
    return c;
}

QString describeConfig(const AppConfig& c)
{
    const auto list = [](const QStringList& l) { return l.isEmpty() ? QStringLiteral("-") : l.join(','); };
    // Plain concatenation: values (paths, labels) may contain '%'.
    const QStringList parts = {
        QStringLiteral("config=") + c.configPath + (c.configLoaded ? QStringLiteral(" (loaded)") : QStringLiteral(" (not found)")),
        QStringLiteral("http_port=") + QString::number(c.httpPort),
        QStringLiteral("ws_port=") + QString::number(c.wsPort),
        QStringLiteral("poll_ms=") + QString::number(c.pollMs),
        QStringLiteral("bind_address=")
            + (c.bindAddress == QHostAddress(QHostAddress::Any) ? QStringLiteral("any") : c.bindAddress.toString()),
        QStringLiteral("metric_prefix=") + c.prom.metricPrefix,
        QStringLiteral("host_label=") + c.prom.hostLabel,
        QStringLiteral("freshness_timeout=") + QString::number(c.prom.freshnessTimeout),
        QStringLiteral("compat_source_label=") + (c.prom.compatSourceLabel ? QStringLiteral("true") : QStringLiteral("false")),
        QStringLiteral("include_sensors=") + list(c.prom.includeSensors),
        QStringLiteral("exclude_sensors=") + list(c.prom.excludeSensors),
        QStringLiteral("include_classes=") + list(c.prom.includeClasses),
        QStringLiteral("exclude_classes=") + list(c.prom.excludeClasses),
        QStringLiteral("include_apps=") + list(c.prom.includeApps),
        QStringLiteral("exclude_apps=") + list(c.prom.excludeApps),
    };
    return parts.join(QLatin1Char(' '));
}
