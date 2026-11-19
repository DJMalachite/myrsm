#include <QCoreApplication>
#include <QDebug>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QJsonDocument>
#include <QUrlQuery>
#include <QTcpServer>
#include <QHttpServer>
#include <QHttpServerResponse>
#include <QHttpServerResponder>
#include <QDateTime>
#include <QSysInfo>
#include <cmath>
#include <QWebSocketServer>
#include <QWebSocket>
#include <windows.h>
#include <tchar.h>
#include "logger.hpp"
#include <QStandardPaths>

using namespace Qt::Literals::StringLiterals;

#include "MyRsm.hpp"
#include "PromExporter.hpp"

#define SERVICE_NAME  _T("MyRsm")

SERVICE_STATUS        g_ServiceStatus = {0};
SERVICE_STATUS_HANDLE g_StatusHandle  = NULL;
HANDLE                g_ServiceStopEvent = INVALID_HANDLE_VALUE;

// Forward declarations
void WINAPI ServiceMain(DWORD argc, LPTSTR *argv);
void WINAPI ServiceCtrlHandler(DWORD ctrlCode);

// Global pointer to QCoreApplication
QCoreApplication *app = nullptr;
quint16 tcpPortArg = 34567;
quint16 wsPortArg = 34568;
int pollIntervalMsArg = 250;
constexpr int kMinPollIntervalMs = 50;
bool runAsServiceFlag = false;
bool stop = false;
prom::Options promOptions;

void loadConfigFromCli(QStringList a) {
    QCommandLineParser parser;
    QCommandLineOption verboseOption(QStringList() << "v" << "version", "Display versions and exit.");
    QCommandLineOption consoleOption(QStringList() << "c" << "console", "Run as console application.");
    QCommandLineOption compatSourceOption("compat-source-label",
        "Append source=\"http\" to sensor series (matches the post-MQTT connector's extra label).");

    parser.addOptions({
            { "httpport", QCoreApplication::translate("main", "The port the http server listens on."),
              "httpport" },
            { "wsport", QCoreApplication::translate("wsport", "The port the websocket server listens on."),
              "wsport" },
            { "pollms", QCoreApplication::translate("main", "Shared memory poll interval in milliseconds (min 50, default 250)."),
              "pollms" },
            { "metric-prefix", "Prometheus metric name prefix (default hwinfo).", "prefix" },
            { "host-label", "Value of the host label on /metrics (default %COMPUTERNAME%).", "name" },
            { "freshness-timeout", "Seconds without a fresh HWiNFO sample before /metrics drops sensor series (default 20).",
              "seconds" },
            { "include-sensors", "Comma-separated, case-insensitive substrings; only matching sensor names are exported.", "list" },
            { "exclude-sensors", "Comma-separated, case-insensitive substrings; matching sensor names are not exported.", "list" },
            { "include-classes", "Comma-separated, case-insensitive substrings; only matching sensor classes are exported.", "list" },
            { "exclude-classes", "Comma-separated, case-insensitive substrings; matching sensor classes are not exported.", "list" },
            { "include-apps", "Comma-separated, case-insensitive substrings; only matching sensor apps are exported.", "list" },
            { "exclude-apps", "Comma-separated, case-insensitive substrings; matching sensor apps are not exported.", "list" },
            compatSourceOption,
            verboseOption,
            consoleOption
    });
    parser.addHelpOption();
    parser.process(a);
    if (parser.isSet("metric-prefix"))
        promOptions.metricPrefix = parser.value("metric-prefix");
    promOptions.hostLabel = parser.isSet("host-label") ? parser.value("host-label")
                                                       : qEnvironmentVariable("COMPUTERNAME");
    if (promOptions.hostLabel.isEmpty() && !parser.isSet("host-label"))
        promOptions.hostLabel = QSysInfo::machineHostName();
    if (parser.isSet("freshness-timeout")) {
        bool ok = false;
        const double t = parser.value("freshness-timeout").toDouble(&ok);
        if (ok && t > 0 && std::isfinite(t))
            promOptions.freshnessTimeout = t;
        else
            qWarning() << "Ignoring invalid --freshness-timeout" << parser.value("freshness-timeout");
    }
    promOptions.includeSensors = prom::parseFilterList(parser.value("include-sensors"));
    promOptions.excludeSensors = prom::parseFilterList(parser.value("exclude-sensors"));
    promOptions.includeClasses = prom::parseFilterList(parser.value("include-classes"));
    promOptions.excludeClasses = prom::parseFilterList(parser.value("exclude-classes"));
    promOptions.includeApps = prom::parseFilterList(parser.value("include-apps"));
    promOptions.excludeApps = prom::parseFilterList(parser.value("exclude-apps"));
    promOptions.compatSourceLabel = parser.isSet(compatSourceOption);
    if (!parser.value("httpport").isEmpty())
        tcpPortArg = parser.value("httpport").toUShort();
    if (!parser.value("wsport").isEmpty())
        wsPortArg = parser.value("wsport").toUShort();
    if (!parser.value("pollms").isEmpty()) {
        int ms = parser.value("pollms").toInt();
        if (ms < kMinPollIntervalMs) {
            qWarning() << "--pollms" << ms << "is below the" << kMinPollIntervalMs << "ms minimum; clamping.";
            ms = kMinPollIntervalMs;
        }
        pollIntervalMsArg = ms;
    }
    if (parser.isSet(verboseOption)) {
        QTextStream(stdout) << VERSION << " " << PROJECT_VERSION_NAME << Qt::endl;
        stop = true;
        return ;
    }
    runAsServiceFlag = GetConsoleWindow() == NULL;
    if (parser.isSet(consoleOption)) {
        runAsServiceFlag = false;
    }
}

static double nowSeconds()
{
    return QDateTime::currentMSecsSinceEpoch() / 1000.0;
}

// /healthz and /readyz: the connector's JSON shape, 200 if exporter_up else 503.
static QHttpServerResponse healthResponse(const MyRsm* rsm)
{
    const double now = nowSeconds();
    const QList<prom::Row> rows = prom::buildRows(rsm->snapshot(), promOptions);
    const prom::Health health = prom::healthFromPoll(rsm->health(), int(rows.size()));
    bool up = false;
    const QByteArray body = prom::renderHealthJson(health, promOptions, now, &up);
    return QHttpServerResponse(QByteArray(prom::kHealthContentType), body,
                               up ? QHttpServerResponder::StatusCode::Ok
                                  : QHttpServerResponder::StatusCode::ServiceUnavailable);
}

int configure()
{
    auto rsm = new MyRsm(pollIntervalMsArg, promOptions.freshnessTimeout, QCoreApplication::instance());
    auto server = new QTcpServer(QCoreApplication::instance());
    auto httpServer = new QHttpServer(QCoreApplication::instance());
    auto wsServer = new QWebSocketServer(QStringLiteral("MyRsm WebSocket Server"), QWebSocketServer::NonSecureMode, QCoreApplication::instance());

    if (!server->listen(QHostAddress::Any, tcpPortArg)) {
        qCritical() << "Failed to start server:" << server->errorString();
        return 1;
    }

    httpServer->route("/", QHttpServerRequest::Method::Get, [rsm](const QHttpServerRequest &request) {
        auto items = request.query().queryItems();
        if (!items.isEmpty())
            qDebug() << "New HTTP connection from" << items;
        return QJsonDocument(rsm->toJson(items)).toJson(QJsonDocument::Compact);
    });

    // Prometheus exposition (port of the HWINFOtoPrometheusConnector output).
    // Query parameters are ignored; filtering comes from the CLI options.
    httpServer->route("/metrics", QHttpServerRequest::Method::Get, [rsm](const QHttpServerRequest &) {
        const double now = nowSeconds();
        const QList<prom::Row> rows = prom::buildRows(rsm->snapshot(), promOptions);
        const prom::Health health = prom::healthFromPoll(rsm->health(), int(rows.size()));
        return QHttpServerResponse(QByteArray(prom::kContentType),
                                   prom::render(rows, health, promOptions, now));
    });
    httpServer->route("/healthz", QHttpServerRequest::Method::Get, [rsm](const QHttpServerRequest &) {
        return healthResponse(rsm);
    });
    httpServer->route("/readyz", QHttpServerRequest::Method::Get, [rsm](const QHttpServerRequest &) {
        return healthResponse(rsm);
    });

    if (!httpServer->bind(server)) {
        qCritical() << "Failed to bind HTTP server to TCP server.";
        return 1;
    }
    qDebug() << "Http server listening on port" << server->serverPort();

    if (!wsServer->listen(QHostAddress::Any, wsPortArg)) {
        qCritical() << "Failed to start WebSocket server:" << wsServer->errorString();
        return 1;
    }

    QObject::connect(wsServer, &QWebSocketServer::newConnection, [wsServer, rsm]() {
        QWebSocket *socket = wsServer->nextPendingConnection();
        auto items = QUrlQuery(socket->requestUrl()).queryItems();
        qDebug() << "New WebSocket connection from" << socket->peerAddress().toString() << ":" << socket->peerPort() << items;
        socket->sendTextMessage(QString::fromUtf8(QJsonDocument(rsm->toJson(items)).toJson(QJsonDocument::Compact)));
        QObject::connect(rsm, &MyRsm::newData, socket, [socket, rsm, items]() {
            if (socket->isValid()) {
                socket->sendTextMessage(QString::fromUtf8(QJsonDocument(rsm->toJson(items)).toJson(QJsonDocument::Compact)));
            }
        });

        QObject::connect(socket, &QWebSocket::disconnected, [socket]() {
            qDebug() << "WebSocket disconnected from" << socket->peerAddress().toString() << ":" << socket->peerPort();
            socket->deleteLater();
        });
    });
    return 0;
}

//------------------------------------------------------------
// Normal console mode
//------------------------------------------------------------
int runAsConsole()
{
    int fake_argc = 0;
    QCoreApplication app(fake_argc, nullptr);

    auto r = configure();
    if (r != 0)
        return r;
    return app.exec();
}

//------------------------------------------------------------
// Windows service mode
//------------------------------------------------------------
void runAsService()
{
    logger::init();
    SERVICE_TABLE_ENTRY ServiceTable[] =
    {
        { (LPWSTR)SERVICE_NAME, (LPSERVICE_MAIN_FUNCTION)ServiceMain },
        { NULL, NULL }
    };

    if (StartServiceCtrlDispatcher(ServiceTable) == FALSE)
        qDebug() << "Failed to connect to SCM:" << GetLastError();
}

static inline QString qstringFromTChar(const TCHAR* t)
{
#ifdef UNICODE
    qDebug() << "Converting from wide char";
    return QString::fromWCharArray(t);
#else
    qDebug() << "Converting from local 8 bit";
    return QString::fromLocal8Bit(t);
#endif
}

int main(int argc, char **argv)
{
    qSetMessagePattern(u"%{category}:%{type}:%{time yyyyMMdd h:mm:ss.zzz "
                    "t}:(%{file}:%{line}):%{function}:%{message}"_s);
    QStringList arguments;
    for (int i = 0; i < argc; ++i) {
        arguments << argv[i];
    }
    loadConfigFromCli(arguments);
    if (stop)
        return 0;
    if (runAsServiceFlag == false) {
        qDebug() << "Starting console application...";
        return runAsConsole();
    }
    qDebug() << "Starting service...";
    runAsService();
    return 0;
}

void WINAPI ServiceMain(DWORD argc, LPTSTR *argv)
{
    logger::init();
    qDebug() << "ServiceMain started.";
    int fake_argc = 0;
    QCoreApplication app(fake_argc, nullptr);
    g_StatusHandle = RegisterServiceCtrlHandler(SERVICE_NAME, ServiceCtrlHandler);

    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    g_ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    auto r = configure();
    if (r == 0) {
            std::thread([&]() {
            WaitForSingleObject(g_ServiceStopEvent, INFINITE);
            QMetaObject::invokeMethod(&app, "quit", Qt::QueuedConnection);
        }).detach();
        app.exec();
    }
    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    logger::uninit();
}

void WINAPI ServiceCtrlHandler(DWORD ctrlCode)
{
    if (ctrlCode == SERVICE_CONTROL_STOP)
        SetEvent(g_ServiceStopEvent);
}
