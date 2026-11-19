#pragma once

// MyRsm runtime configuration: defaults < ini file < command line.
//
// The ini file (default %ProgramData%\MyRsm\myrsm.ini, override with
// --config) lets the Windows service be reconfigured with an edit and a
// service restart: the service binPath carries no arguments.
//
//   [myrsm]
//   http_port=34567
//   ws_port=34568
//   poll_ms=250
//   bind_address=            ; empty/"any" = all interfaces, or an IP address
//   metric_prefix=hwinfo
//   host_label=              ; empty = %COMPUTERNAME%
//   freshness_timeout=20
//   compat_source_label=false
//   include_sensors=         ; comma-separated, case-insensitive substrings
//   exclude_sensors=
//   include_classes=
//   exclude_classes=
//   include_apps=
//   exclude_apps=

#include <QCommandLineParser>
#include <QHostAddress>
#include <QString>
#include <QStringList>

#include "PromExporter.hpp"

struct AppConfig {
    quint16 httpPort = 34567;
    quint16 wsPort = 34568;
    int pollMs = 250;
    QHostAddress bindAddress = QHostAddress(QHostAddress::Any);
    prom::Options prom;            // hostLabel resolved (never empty unless set so explicitly)
    QString configPath;            // ini file that was looked for
    bool configLoaded = false;     // ini file existed and was read
};

inline constexpr int kMinPollIntervalMs = 50;

// %ProgramData%\MyRsm\myrsm.ini
QString defaultConfigPath();

// Registers every config option (plus --config) on the parser.
void addConfigOptions(QCommandLineParser& parser);

// Applies defaults, then the ini file (from --config or defaultConfigPath()),
// then command-line values. Invalid values are ignored (keeping the lower
// precedence value) and reported in *warnings.
AppConfig resolveConfig(const QCommandLineParser& parser, QStringList* warnings);

// Lower-level pieces, exposed for tests.
void applyIniFile(const QString& path, AppConfig* config, QStringList* warnings);
void applyCommandLine(const QCommandLineParser& parser, AppConfig* config, QStringList* warnings);

// One-line summary for the log.
QString describeConfig(const AppConfig& config);
