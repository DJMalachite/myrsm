// Tests for the ini + command-line configuration (Config.cpp):
// precedence CLI > ini > defaults, value validation, and the file format the
// installer writes (plain key=value under [myrsm]).

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "Config.hpp"

namespace {

QString writeIni(const QTemporaryDir& dir, const QByteArray& content, const QString& name = "myrsm.ini")
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        qFatal("cannot write %s", qPrintable(path));
    f.write(content);
    return path;
}

AppConfig resolve(const QStringList& args, QStringList* warnings)
{
    QCommandLineParser parser;
    addConfigOptions(parser);
    QStringList argv = {"myrsm"};
    argv += args;
    if (!parser.parse(argv))
        qFatal("parse failed: %s", qPrintable(parser.errorText()));
    return resolveConfig(parser, warnings);
}

bool anyContains(const QStringList& list, const QString& needle)
{
    for (const QString& s : list)
        if (s.contains(needle))
            return true;
    return false;
}

} // namespace

class TestConfig : public QObject
{
    Q_OBJECT
private slots:
    void init() { qputenv("COMPUTERNAME", "TESTPC"); }
    void defaultsWithoutFile();
    void missingExplicitConfigWarns();
    void allKeys();
    void installerStyleFile();
    void commaListsAndSpaces();
    void invalidValuesKeepDefaults();
    void unknownAndMisplacedKeys();
    void commandLineOverridesIni();
    void hostLabelDefaults();
    void pollClamp();
    void samePortsWarn();
    void bindAddress();
    void describe();
};

void TestConfig::defaultsWithoutFile()
{
    QTemporaryDir dir;
    QStringList w;
    const AppConfig c = resolve({"--config", dir.filePath("absent.ini")}, &w);
    QCOMPARE(c.httpPort, quint16(34567));
    QCOMPARE(c.wsPort, quint16(34568));
    QCOMPARE(c.pollMs, 250);
    QCOMPARE(c.bindAddress, QHostAddress(QHostAddress::Any));
    QCOMPARE(c.prom.metricPrefix, QStringLiteral("hwinfo"));
    QCOMPARE(c.prom.hostLabel, QStringLiteral("TESTPC"));
    QCOMPARE(c.prom.freshnessTimeout, 20.0);
    QCOMPARE(c.prom.compatSourceLabel, false);
    QVERIFY(c.prom.includeSensors.isEmpty());
    QVERIFY(!c.configLoaded);
}

void TestConfig::missingExplicitConfigWarns()
{
    QTemporaryDir dir;
    QStringList w;
    resolve({"--config", dir.filePath("absent.ini")}, &w);
    QVERIFY(anyContains(w, "file not found"));
}

void TestConfig::allKeys()
{
    QTemporaryDir dir;
    const QString path = writeIni(dir,
        "[myrsm]\n"
        "http_port=40000\n"
        "ws_port=40001\n"
        "poll_ms=500\n"
        "bind_address=127.0.0.1\n"
        "metric_prefix=hwinfo_shadow\n"
        "host_label=DJMalachite\n"
        "freshness_timeout=45.5\n"
        "compat_source_label=true\n"
        "include_sensors=clock\n"
        "exclude_sensors=temp\n"
        "include_classes=gpu\n"
        "exclude_classes=network\n"
        "include_apps=hwinfo\n"
        "exclude_apps=aida\n");
    QStringList w;
    const AppConfig c = resolve({"--config", path}, &w);
    QVERIFY2(w.isEmpty(), qPrintable(w.join('\n')));
    QVERIFY(c.configLoaded);
    QCOMPARE(c.httpPort, quint16(40000));
    QCOMPARE(c.wsPort, quint16(40001));
    QCOMPARE(c.pollMs, 500);
    QCOMPARE(c.bindAddress, QHostAddress(QStringLiteral("127.0.0.1")));
    QCOMPARE(c.prom.metricPrefix, QStringLiteral("hwinfo_shadow"));
    QCOMPARE(c.prom.hostLabel, QStringLiteral("DJMalachite"));
    QCOMPARE(c.prom.freshnessTimeout, 45.5);
    QCOMPARE(c.prom.compatSourceLabel, true);
    QCOMPARE(c.prom.includeSensors, QStringList{"clock"});
    QCOMPARE(c.prom.excludeSensors, QStringList{"temp"});
    QCOMPARE(c.prom.includeClasses, QStringList{"gpu"});
    QCOMPARE(c.prom.excludeClasses, QStringList{"network"});
    QCOMPARE(c.prom.includeApps, QStringList{"hwinfo"});
    QCOMPARE(c.prom.excludeApps, QStringList{"aida"});
}

void TestConfig::installerStyleFile()
{
    // What Inno Setup's SetIniString writes: CRLF, no quoting, possibly an
    // empty value, keys in installer order.
    QTemporaryDir dir;
    const QString path = writeIni(dir,
        "[myrsm]\r\n"
        "http_port=34567\r\n"
        "ws_port=34568\r\n"
        "poll_ms=250\r\n"
        "host_label=\r\n"
        "metric_prefix=hwinfo\r\n"
        "compat_source_label=0\r\n");
    QStringList w;
    const AppConfig c = resolve({"--config", path}, &w);
    QVERIFY2(w.isEmpty(), qPrintable(w.join('\n')));
    QCOMPARE(c.httpPort, quint16(34567));
    QCOMPARE(c.prom.hostLabel, QStringLiteral("TESTPC")); // empty = default
    QCOMPARE(c.prom.compatSourceLabel, false);
}

void TestConfig::commaListsAndSpaces()
{
    QTemporaryDir dir;
    const QString path = writeIni(dir,
        "[myrsm]\n"
        "include_sensors=GPU, Core Clock ,,temp\n"
        "exclude_classes=\"a, b\"\n"
        "host_label=  My PC  \n");
    QStringList w;
    const AppConfig c = resolve({"--config", path}, &w);
    QVERIFY2(w.isEmpty(), qPrintable(w.join('\n')));
    QCOMPARE(c.prom.includeSensors, (QStringList{"gpu", "core clock", "temp"}));
    QCOMPARE(c.prom.excludeClasses, (QStringList{"a", "b"}));
    QCOMPARE(c.prom.hostLabel, QStringLiteral("My PC"));
}

void TestConfig::invalidValuesKeepDefaults()
{
    QTemporaryDir dir;
    const QString path = writeIni(dir,
        "[myrsm]\n"
        "http_port=0\n"
        "ws_port=70000\n"
        "poll_ms=fast\n"
        "bind_address=not-an-ip\n"
        "freshness_timeout=-1\n"
        "compat_source_label=maybe\n");
    QStringList w;
    const AppConfig c = resolve({"--config", path}, &w);
    QCOMPARE(c.httpPort, quint16(34567));
    QCOMPARE(c.wsPort, quint16(34568));
    QCOMPARE(c.pollMs, 250);
    QCOMPARE(c.bindAddress, QHostAddress(QHostAddress::Any));
    QCOMPARE(c.prom.freshnessTimeout, 20.0);
    QCOMPARE(c.prom.compatSourceLabel, false);
    QCOMPARE(w.size(), 6);
    QVERIFY(anyContains(w, "http_port=\"0\""));
    QVERIFY(anyContains(w, "ws_port=\"70000\""));
}

void TestConfig::unknownAndMisplacedKeys()
{
    QTemporaryDir dir;
    const QString path = writeIni(dir,
        "toplevel=1\n"
        "[other]\n"
        "x=1\n"
        "[myrsm]\n"
        "http_prot=1234\n");
    QStringList w;
    const AppConfig c = resolve({"--config", path}, &w);
    QCOMPARE(c.httpPort, quint16(34567));
    QVERIFY(anyContains(w, "unknown key \"http_prot\""));
    QVERIFY(anyContains(w, "outside the [myrsm] section"));
    QVERIFY(anyContains(w, "section [other]"));
}

void TestConfig::commandLineOverridesIni()
{
    QTemporaryDir dir;
    const QString path = writeIni(dir,
        "[myrsm]\n"
        "http_port=40000\n"
        "metric_prefix=from_ini\n"
        "host_label=IniHost\n"
        "include_sensors=ini\n"
        "freshness_timeout=30\n");
    QStringList w;
    const AppConfig c = resolve({"--config", path, "--httpport", "41000", "--metric-prefix", "from_cli",
                                 "--include-sensors", "", "--compat-source-label"}, &w);
    QVERIFY2(w.isEmpty(), qPrintable(w.join('\n')));
    QCOMPARE(c.httpPort, quint16(41000));                  // CLI wins
    QCOMPARE(c.prom.metricPrefix, QStringLiteral("from_cli"));
    QCOMPARE(c.prom.hostLabel, QStringLiteral("IniHost"));  // ini wins over default
    QVERIFY(c.prom.includeSensors.isEmpty());               // explicit empty CLI clears it
    QCOMPARE(c.prom.freshnessTimeout, 30.0);
    QCOMPARE(c.prom.compatSourceLabel, true);

    // An invalid CLI value keeps the ini value.
    QStringList w2;
    const AppConfig c2 = resolve({"--config", path, "--httpport", "nope"}, &w2);
    QCOMPARE(c2.httpPort, quint16(40000));
    QVERIFY(anyContains(w2, "--httpport"));
}

void TestConfig::hostLabelDefaults()
{
    QTemporaryDir dir;
    QStringList w;
    // Explicit empty --host-label is honoured.
    const AppConfig c = resolve({"--config", dir.filePath("absent.ini"), "--host-label", ""}, &w);
    QCOMPARE(c.prom.hostLabel, QString());
    // Without COMPUTERNAME, falls back to the machine host name.
    qunsetenv("COMPUTERNAME");
    const AppConfig c2 = resolve({"--config", dir.filePath("absent.ini")}, &w);
    QCOMPARE(c2.prom.hostLabel, QSysInfo::machineHostName());
}

void TestConfig::pollClamp()
{
    QTemporaryDir dir;
    const QString path = writeIni(dir, "[myrsm]\npoll_ms=10\n");
    QStringList w;
    QCOMPARE(resolve({"--config", path}, &w).pollMs, kMinPollIntervalMs);
    QVERIFY(anyContains(w, "clamping"));
    QStringList w2;
    QCOMPARE(resolve({"--config", path, "--pollms", "20"}, &w2).pollMs, kMinPollIntervalMs);
}

void TestConfig::samePortsWarn()
{
    QTemporaryDir dir;
    QStringList w;
    resolve({"--config", dir.filePath("absent.ini"), "--httpport", "5000", "--wsport", "5000"}, &w);
    QVERIFY(anyContains(w, "both 5000"));
}

void TestConfig::bindAddress()
{
    QTemporaryDir dir;
    QStringList w;
    QCOMPARE(resolve({"--config", dir.filePath("a.ini"), "--bind-address", "any"}, &w).bindAddress,
             QHostAddress(QHostAddress::Any));
    QCOMPARE(resolve({"--config", dir.filePath("a.ini"), "--bind-address", "::1"}, &w).bindAddress,
             QHostAddress(QStringLiteral("::1")));
    QCOMPARE(resolve({"--config", dir.filePath("a.ini"), "--bind-address", "0.0.0.0"}, &w).bindAddress,
             QHostAddress(QStringLiteral("0.0.0.0")));
}

void TestConfig::describe()
{
    QTemporaryDir dir;
    QStringList w;
    const AppConfig c = resolve({"--config", dir.filePath("a.ini"), "--host-label", "100%PC"}, &w);
    const QString d = describeConfig(c);
    QVERIFY2(d.contains("host_label=100%PC"), qPrintable(d));
    QVERIFY(d.contains("http_port=34567"));
    QVERIFY(d.contains("(not found)"));
}

QTEST_GUILESS_MAIN(TestConfig)
#include "tst_config.moc"
