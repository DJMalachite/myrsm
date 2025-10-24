#pragma once

#include <QDebug>
#include <QDate>
#include <QString>
#include <QMutex>
#include <QFile>

class logger
{
    public:
        static void init();
        static void uninit();
        ~logger();

    private:
        QMutex m;
        logger();
        void initLogFileName();
        void deleteOldLogs();
        QString logFileName;
        QString logFolderName;
        QDate logDate;
        int max_logs_files;
        QFile _outFile;
        bool std_err_log;
        void _handler(QtMsgType type, const QMessageLogContext &context, const QString& msg);

        static logger* _inst;
        static void handler(QtMsgType type, const QMessageLogContext &context, const QString& msg);
};
