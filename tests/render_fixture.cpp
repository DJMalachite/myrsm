// myrsm_render_fixture: renders a MyRSM "/" JSON fixture through the same
// prom::buildRows + prom::render code the /metrics route uses, with a frozen
// poll state. tools/run_parity.py feeds the identical fixture and frozen state
// to the reference Python connector and diffs the two outputs.
//
// The frozen state MUST stay in sync with tools/reference/render_reference.py.

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QTextStream>

#include "FixtureLoader.hpp"
#include "PromExporter.hpp"

namespace {
constexpr double kNow = 1700000000.0;          // frozen time.time()
constexpr double kPollDuration = 0.005;        // frozen last poll duration
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addPositionalArgument("fixture", "MyRSM / JSON fixture to render.");
    parser.addOptions({
        {"out", "Write the exposition to this file (UTF-8, LF).", "file"},
        {"metric-prefix", "Metric prefix.", "prefix", "hwinfo"},
        {"host-label", "host label value.", "name", "fixture-host"},
        {"freshness-timeout", "Freshness timeout (s).", "seconds", "20"},
        {"data-age", "Seconds since the frozen last successful poll.", "seconds", "1"},
        {"include-sensors", "Filter list.", "list"},
        {"exclude-sensors", "Filter list.", "list"},
        {"include-classes", "Filter list.", "list"},
        {"exclude-classes", "Filter list.", "list"},
        {"include-apps", "Filter list.", "list"},
        {"exclude-apps", "Filter list.", "list"},
        {"compat-source-label", "Append source=\"http\"."},
    });
    parser.addHelpOption();
    parser.process(app);

    QTextStream err(stderr);
    if (parser.positionalArguments().size() != 1 || !parser.isSet("out")) {
        err << "usage: myrsm_render_fixture <fixture.json> --out <file> [options]\n";
        return 2;
    }

    QFile in(parser.positionalArguments().first());
    if (!in.open(QIODevice::ReadOnly)) {
        err << "cannot open " << in.fileName() << "\n";
        return 2;
    }
    QList<SnapshotRow> snapshot;
    int rawCount = 0;
    QString error;
    if (!loadFixture(in.readAll(), &snapshot, &rawCount, &error)) {
        err << in.fileName() << ": " << error << "\n";
        return 2;
    }

    prom::Options o;
    o.metricPrefix = parser.value("metric-prefix");
    o.hostLabel = parser.value("host-label");
    o.freshnessTimeout = parser.value("freshness-timeout").toDouble();
    o.includeSensors = prom::parseFilterList(parser.value("include-sensors"));
    o.excludeSensors = prom::parseFilterList(parser.value("exclude-sensors"));
    o.includeClasses = prom::parseFilterList(parser.value("include-classes"));
    o.excludeClasses = prom::parseFilterList(parser.value("exclude-classes"));
    o.includeApps = prom::parseFilterList(parser.value("include-apps"));
    o.excludeApps = prom::parseFilterList(parser.value("exclude-apps"));
    o.compatSourceLabel = parser.isSet("compat-source-label");

    const QList<prom::Row> rows = prom::buildRows(snapshot, o);

    // Same rule as the connector's poll_once(): zero exported rows is a failed
    // poll (and, never having succeeded, a stale one).
    prom::Health h;
    h.lastAttemptTs = kNow - 1.0;
    h.lastPollDuration = kPollDuration;
    h.httpStatus = 200;
    h.rawItems = rawCount;
    if (!rows.isEmpty()) {
        h.sourceUp = true;
        h.lastSuccessTs = kNow - parser.value("data-age").toDouble();
        h.ok = 1;
        h.failed = 0;
        h.exportedItems = int(rows.size());
    } else {
        h.sourceUp = false;
        h.lastSuccessTs = 0.0;
        h.ok = 0;
        h.failed = 1;
        h.exportedItems = 0;
    }

    QFile out(parser.value("out"));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        err << "cannot write " << out.fileName() << "\n";
        return 2;
    }
    out.write(prom::render(rows, h, o, kNow));
    return 0;
}
