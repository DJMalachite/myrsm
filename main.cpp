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
#include <QTextStream>
#include <QWebSocketServer>
#include <QWebSocket>
#include <windows.h>
#include <thread>
#include "logger.hpp"

using namespace Qt::Literals::StringLiterals;

#include "Config.hpp"
#include "MyRsm.hpp"
#include "PromExporter.hpp"
#include "ServiceControl.hpp"

namespace {

// configure() result codes; reported as the service-specific exit code.
constexpr int kConfigureOk = 0;
constexpr int kHttpListenFailed = 1;
constexpr int kHttpBindFailed = 2;
constexpr int kWsListenFailed = 3;

constexpr DWORD kStartWaitHintMs = 10000;
constexpr DWORD kStopWaitHintMs = 10000;

AppConfig g_config;

SERVICE_STATUS g_serviceStatus = {};
SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
HANDLE g_serviceStopEvent = nullptr;
DWORD g_checkPoint = 1;

void WINAPI serviceMain(DWORD argc, LPWSTR* argv);
DWORD WINAPI serviceCtrlHandler(DWORD control, DWORD eventType, LPVOID eventData, LPVOID context);

double nowSeconds()
{
    return QDateTime::currentMSecsSinceEpoch() / 1000.0;
}

// /healthz and /readyz: the connector's JSON shape, 200 if exporter_up else 503.
QHttpServerResponse healthResponse(const MyRsm* rsm)
{
    const double now = nowSeconds();
    const QList<prom::Row> rows = prom::buildRows(rsm->snapshot(), g_config.prom);
    const prom::Health health = prom::healthFromPoll(rsm->health(), int(rows.size()));
    bool up = false;
    const QByteArray body = prom::renderHealthJson(health, g_config.prom, now, &up);
    return QHttpServerResponse(QByteArray(prom::kHealthContentType), body,
                               up ? QHttpServerResponder::StatusCode::Ok
                                  : QHttpServerResponder::StatusCode::ServiceUnavailable);
}

// Stands up MyRsm and both servers. Shared by console and service mode.
int configure()
{
    auto rsm = new MyRsm(g_config.pollMs, g_config.prom.freshnessTimeout, QCoreApplication::instance());
    auto server = new QTcpServer(QCoreApplication::instance());
    auto httpServer = new QHttpServer(QCoreApplication::instance());
    auto wsServer = new QWebSocketServer(QStringLiteral("MyRsm WebSocket Server"), QWebSocketServer::NonSecureMode, QCoreApplication::instance());

    if (!server->listen(g_config.bindAddress, g_config.httpPort)) {
        qCritical() << "Failed to start server:" << server->errorString();
        return kHttpListenFailed;
    }

    httpServer->route("/", QHttpServerRequest::Method::Get, [rsm](const QHttpServerRequest &request) {
        auto items = request.query().queryItems();
        if (!items.isEmpty())
            qDebug() << "New HTTP connection from" << items;
        return QJsonDocument(rsm->toJson(items)).toJson(QJsonDocument::Compact);
    });

    // Prometheus exposition (port of the HWINFOtoPrometheusConnector output).
    // Query parameters are ignored; filtering comes from the configuration.
    httpServer->route("/metrics", QHttpServerRequest::Method::Get, [rsm](const QHttpServerRequest &) {
        const double now = nowSeconds();
        const QList<prom::Row> rows = prom::buildRows(rsm->snapshot(), g_config.prom);
        const prom::Health health = prom::healthFromPoll(rsm->health(), int(rows.size()));
        return QHttpServerResponse(QByteArray(prom::kContentType),
                                   prom::render(rows, health, g_config.prom, now));
    });
    httpServer->route("/healthz", QHttpServerRequest::Method::Get, [rsm](const QHttpServerRequest &) {
        return healthResponse(rsm);
    });
    httpServer->route("/readyz", QHttpServerRequest::Method::Get, [rsm](const QHttpServerRequest &) {
        return healthResponse(rsm);
    });

    if (!httpServer->bind(server)) {
        qCritical() << "Failed to bind HTTP server to TCP server.";
        return kHttpBindFailed;
    }
    qDebug() << "Http server listening on port" << server->serverPort();

    if (!wsServer->listen(g_config.bindAddress, g_config.wsPort)) {
        qCritical() << "Failed to start WebSocket server:" << wsServer->errorString();
        return kWsListenFailed;
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
    return kConfigureOk;
}

//------------------------------------------------------------
// Normal console mode
//------------------------------------------------------------
int runAsConsole()
{
    int fake_argc = 0;
    QCoreApplication app(fake_argc, nullptr);

    auto r = configure();
    if (r != kConfigureOk)
        return r;
    return app.exec();
}

//------------------------------------------------------------
// Windows service mode
//------------------------------------------------------------
void reportServiceStatus(DWORD state, DWORD win32ExitCode = NO_ERROR, DWORD serviceExitCode = 0,
                         DWORD waitHintMs = 0)
{
    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_serviceStatus.dwCurrentState = state;
    g_serviceStatus.dwWin32ExitCode = win32ExitCode;
    g_serviceStatus.dwServiceSpecificExitCode = serviceExitCode;
    g_serviceStatus.dwWaitHint = waitHintMs;
    // No controls while starting; STOP/SHUTDOWN otherwise.
    g_serviceStatus.dwControlsAccepted =
        state == SERVICE_START_PENDING ? 0 : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
    g_serviceStatus.dwCheckPoint =
        (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : g_checkPoint++;
    if (g_statusHandle)
        SetServiceStatus(g_statusHandle, &g_serviceStatus);
}

void runAsService()
{
    SERVICE_TABLE_ENTRYW serviceTable[] = {
        {const_cast<LPWSTR>(service_control::kServiceName), serviceMain},
        {nullptr, nullptr},
    };
    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        const DWORD e = GetLastError();
        qCritical() << "Failed to connect to the service control manager:" << e;
        if (e == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
            qCritical() << "myrsm.exe was started without a console and not by the SCM; use --console"
                           " or 'myrsm.exe service install'.";
    }
}

void WINAPI serviceMain(DWORD, LPWSTR*)
{
    qInfo() << "ServiceMain started.";
    g_statusHandle = RegisterServiceCtrlHandlerExW(service_control::kServiceName, serviceCtrlHandler, nullptr);
    if (!g_statusHandle) {
        qCritical() << "RegisterServiceCtrlHandlerExW failed:" << GetLastError();
        return;
    }
    reportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 0, kStartWaitHintMs);

    g_serviceStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_serviceStopEvent) {
        const DWORD e = GetLastError();
        qCritical() << "CreateEventW failed:" << e;
        reportServiceStatus(SERVICE_STOPPED, e);
        return;
    }

    int result = kConfigureOk;
    {
        int fake_argc = 0;
        QCoreApplication app(fake_argc, nullptr);
        result = configure();
        if (result == kConfigureOk) {
            std::thread waiter([&app]() {
                WaitForSingleObject(g_serviceStopEvent, INFINITE);
                QMetaObject::invokeMethod(&app, "quit", Qt::QueuedConnection);
            });
            // RUNNING only once the servers are actually listening.
            reportServiceStatus(SERVICE_RUNNING);
            qInfo() << "Service running.";
            app.exec();
            SetEvent(g_serviceStopEvent); // in case exec() returned on its own
            waiter.join();
            qInfo() << "Service stopping.";
        } else {
            qCritical() << "Service failed to start, exit code" << result;
        }
        // app and everything parented to it are destroyed here, before we
        // tell the SCM we're stopped.
    }
    CloseHandle(g_serviceStopEvent);
    g_serviceStopEvent = nullptr;

    if (result == kConfigureOk)
        reportServiceStatus(SERVICE_STOPPED);
    else
        reportServiceStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, DWORD(result));
}

DWORD WINAPI serviceCtrlHandler(DWORD control, DWORD, LPVOID, LPVOID)
{
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        reportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 0, kStopWaitHintMs);
        if (g_serviceStopEvent)
            SetEvent(g_serviceStopEvent);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

} // namespace

int main(int argc, char **argv)
{
    qSetMessagePattern(u"%{category}:%{type}:%{time yyyyMMdd h:mm:ss.zzz "
                    "t}:(%{file}:%{line}):%{function}:%{message}"_s);
    // Unicode command line (argv is in the ANSI code page).
    QStringList arguments;
    int wargc = 0;
    if (LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
        for (int i = 0; i < wargc; ++i)
            arguments << QString::fromWCharArray(wargv[i]);
        LocalFree(wargv);
    } else {
        for (int i = 0; i < argc; ++i)
            arguments << QString::fromLocal8Bit(argv[i]);
    }

    // myrsm.exe service <install|uninstall|start|stop|status>
    if (arguments.size() >= 2 && arguments.at(1) == u"service")
        return service_control::run(arguments.mid(2));

    QCommandLineParser parser;
    QCommandLineOption versionOption(QStringList() << "v" << "version", "Display versions and exit.");
    QCommandLineOption consoleOption(QStringList() << "c" << "console", "Run as console application.");
    addConfigOptions(parser);
    parser.addOption(versionOption);
    parser.addOption(consoleOption);
    parser.addHelpOption();
    parser.process(arguments);

    if (parser.isSet(versionOption)) {
        QTextStream(stdout) << VERSION << " " << PROJECT_VERSION_NAME << Qt::endl;
        return 0;
    }

    const bool runAsServiceFlag = GetConsoleWindow() == nullptr && !parser.isSet(consoleOption);
    // Service mode logs to %ProgramData%\MyRsm\logs; initialise the logger
    // once, before anything is logged (config warnings included).
    if (runAsServiceFlag)
        logger::init();

    QStringList warnings;
    g_config = resolveConfig(parser, &warnings);
    for (const QString& w : warnings)
        qWarning().noquote() << w;
    qInfo().noquote() << "MyRsm" << VERSION << PROJECT_VERSION_NAME << describeConfig(g_config);

    int rc = 0;
    if (!runAsServiceFlag) {
        qDebug() << "Starting console application...";
        rc = runAsConsole();
    } else {
        qDebug() << "Starting service...";
        runAsService();
        logger::uninit();
    }
    return rc;
}
