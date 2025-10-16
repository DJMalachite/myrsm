#include "MyRsm.hpp"
#include <QCoreApplication>
#include <QHttpServer>
#include <QCommandLineParser>
#include <QTcpServer>
#include <QJsonDocument>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QTextStream>

using namespace Qt::Literals::StringLiterals;

int main(int ac, char **av)
{
    qSetMessagePattern(u"%{category}:%{type}:%{time yyyyMMdd h:mm:ss.zzz "
                     "t}:(%{file}:%{line}):%{function}:%{message}"_s);
    QCoreApplication a(ac, av);

    QCommandLineParser parser;
    QCommandLineOption verboseOption(QStringList() << "v" << "version", "Display versions and exit.");

    parser.addOptions({
            { "httpport", QCoreApplication::translate("main", "The port the http server listens on."),
              "httpport" },
            { "wsport", QCoreApplication::translate("wsport", "The port the websocket server listens on."),
              "wsport" },
            verboseOption
    });
    parser.addHelpOption();
    parser.process(a);
    quint16 tcpPortArg = 34567;
    quint16 wsPortArg = 34568;
    if (parser.isSet(verboseOption))
    {
        QTextStream(stdout) << VERSION << " " << PROJECT_VERSION_NAME << Qt::endl;
        return 0;
    }
    if (!parser.value("httpport").isEmpty())
        tcpPortArg = parser.value("httpport").toUShort();
    if (!parser.value("wsport").isEmpty())
        wsPortArg = parser.value("wsport").toUShort();
    MyRsm rsm;
    QTcpServer server;
    if (!server.listen(QHostAddress::Any, tcpPortArg)) {
        qCritical() << "Failed to start server:" << server.errorString();
        return 1;
    }

    QHttpServer httpServer;
    httpServer.route("/", QHttpServerRequest::Method::Get, [&rsm](const QHttpServerRequest &request) {
        auto items = request.query().queryItems();
        if (!items.isEmpty())
            qDebug() << "New HTTP connection from" << items;
        return QJsonDocument(rsm.toJson(items)).toJson(QJsonDocument::Compact);
    });

    if (!httpServer.bind(&server)) {
        qCritical() << "Failed to bind HTTP server to TCP server.";
        return 1;
    }
    qDebug() << "Http server listening on port" << server.serverPort();
    QWebSocketServer wsServer(QStringLiteral("MyRsm WebSocket Server"), QWebSocketServer::NonSecureMode);
    if (!wsServer.listen(QHostAddress::Any, wsPortArg)) {
        qCritical() << "Failed to start WebSocket server:" << wsServer.errorString();
        return 1;
    }

    QObject::connect(&wsServer, &QWebSocketServer::newConnection, [&wsServer, &rsm]() {
        QWebSocket *socket = wsServer.nextPendingConnection();
        auto items = QUrlQuery(socket->requestUrl()).queryItems();
        qDebug() << "New WebSocket connection from" << socket->peerAddress().toString() << ":" << socket->peerPort() << items;
        socket->sendTextMessage(QString::fromUtf8(QJsonDocument(rsm.toJson(items)).toJson(QJsonDocument::Compact)));
        QObject::connect(&rsm, &MyRsm::newData, socket, [socket, &rsm, items]() {
            if (socket->isValid()) {
                socket->sendTextMessage(QString::fromUtf8(QJsonDocument(rsm.toJson(items)).toJson(QJsonDocument::Compact)));
            }
        });
        QObject::connect(socket, &QWebSocket::disconnected, socket, [socket]() {
            qDebug() << "WebSocket disconnected from" << socket->peerAddress().toString() << ":" << socket->peerPort();
            socket->deleteLater();
        });
    });
    qDebug() << "WebSocket server listening on port" << wsServer.serverPort();
    return a.exec();
}
