#include <QCoreApplication>
#include <QDebug>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QJsonDocument>
#include <QUrlQuery>
#include <QTcpServer>
#include <QHttpServer>
#include <QWebSocketServer>
#include <QWebSocket>
#include <windows.h>
#include <tchar.h>
#include "logger.hpp"
#include <QStandardPaths>

using namespace Qt::Literals::StringLiterals;

#include "MyRsm.hpp"

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

void loadConfigFromCli(QStringList a) {
    QCommandLineParser parser;
    QCommandLineOption verboseOption(QStringList() << "v" << "version", "Display versions and exit.");
    QCommandLineOption consoleOption(QStringList() << "c" << "console", "Run as console application.");

    parser.addOptions({
            { "httpport", QCoreApplication::translate("main", "The port the http server listens on."),
              "httpport" },
            { "wsport", QCoreApplication::translate("wsport", "The port the websocket server listens on."),
              "wsport" },
            { "pollms", QCoreApplication::translate("main", "Shared memory poll interval in milliseconds (min 50, default 250)."),
              "pollms" },
            verboseOption,
            consoleOption
    });
    parser.addHelpOption();
    parser.process(a);
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

int configure()
{
    auto rsm = new MyRsm(pollIntervalMsArg, 20.0, QCoreApplication::instance());
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
